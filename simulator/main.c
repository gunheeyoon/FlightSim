/* Waypoint-driven C11 simulator. One event loop owns all state: commands are
   applied before the next motion step; no shared state or threads are needed. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "route.h"
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET Socket;
#define BAD_SOCKET INVALID_SOCKET
#define close_socket closesocket
#else
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int Socket;
#define BAD_SOCKET (-1)
#define close_socket close
#endif

#define PI 3.14159265358979323846
#define RAD (PI / 180.0)
#define EARTH 6371000.0
enum { LON, LAT, ALT, SPEED, ROLL, PITCH, YAW };
typedef struct {
    double v[7];
    int frozen[3], altitude_override, speed_override;
    size_t target;
    int finished, registered, initialized, awaiting_command_state;
    char pending_command[128];
    unsigned long command_count, interval_count;
    char history[8][128];
} Simulation;
static volatile sig_atomic_t running = 1;
static void stop(int sig) { (void)sig; running = 0; }
static double now_seconds(void) {
#ifdef _WIN32
    LARGE_INTEGER count, frequency;
    QueryPerformanceCounter(&count); QueryPerformanceFrequency(&frequency);
    return (double)count.QuadPart / (double)frequency.QuadPart;
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
#endif
}
static double clamp(double v, double lo, double hi) { return fmax(lo, fmin(hi, v)); }
static double wrap(double degrees) {
    degrees = fmod(degrees, 360.0);
    return degrees < 0 ? degrees + 360 : degrees;
}
static double difference(double to, double from) { return wrap(to - from + 180) - 180; }
/* Great-circle distance and initial bearing, robust across the date line. */
static void direction(double lon, double lat, const Waypoint *to, double *distance, double *bearing) {
    double a = lat * RAD, b = to->latitude * RAD;
    double dl = difference(to->longitude, lon) * RAD, dp = b - a;
    double h = sin(dp / 2) * sin(dp / 2) + cos(a) * cos(b) * sin(dl / 2) * sin(dl / 2);
    *distance = 2 * EARTH * asin(sqrt(clamp(h, 0, 1)));
    *bearing = wrap(atan2(sin(dl) * cos(b), cos(a) * sin(b) - sin(a) * cos(b) * cos(dl)) / RAD);
}
static void step(Simulation *s, const Route *route, double dt) {
    const Waypoint *target;
    double distance, bearing, travel, fraction, heading_change, desired_roll;
    if (s->finished) return;
    target = &route->points[s->target];
    direction(s->v[LON], s->v[LAT], target, &distance, &bearing);
    if (!s->speed_override) {
        /* Approach the target waypoint's nominal speed smoothly. */
        s->v[SPEED] += clamp(target->airspeed - s->v[SPEED], -5 * dt, 5 * dt);
    }
    travel = fmin(distance, s->v[SPEED] * dt);
    fraction = distance > 0.001 ? travel / distance : 1;
    heading_change = clamp(difference(bearing, s->v[YAW]), -20 * dt, 20 * dt);
    desired_roll = atan(s->v[SPEED] * heading_change * RAD / fmax(dt, 0.001) / 9.80665) / RAD;
    if (!s->frozen[0]) s->v[ROLL] += clamp(clamp(desired_roll, -30, 30) - s->v[ROLL], -30 * dt, 30 * dt);
    if (!s->frozen[1]) s->v[PITCH] = s->altitude_override ? 0 : atan2(target->altitude - s->v[ALT], fmax(distance, 0.001)) / RAD;
    if (!s->frozen[2]) s->v[YAW] = wrap(s->v[YAW] + heading_change);
    /* Move along the great circle. Attitude is illustrative; frozen angles do
       not change the waypoint track in this kinematic testing model. */
    if (distance > 0.001 && travel > 0) {
        double a = s->v[LAT] * RAD, d = travel / EARTH, b = bearing * RAD;
        double next_lat = asin(clamp(sin(a) * cos(d) + cos(a) * sin(d) * cos(b), -1, 1));
        s->v[LON] = difference(s->v[LON] + atan2(sin(b) * sin(d) * cos(a), cos(d) - sin(a) * sin(next_lat)) / RAD, 0);
        s->v[LAT] = next_lat / RAD;
    }
    if (!s->altitude_override) s->v[ALT] += (target->altitude - s->v[ALT]) * fraction;
    if (distance <= 0.001 || travel >= distance) {
        s->v[LON] = target->longitude; s->v[LAT] = target->latitude;
        if (!s->altitude_override) s->v[ALT] = target->altitude;
        if (++s->target == route->count) {
            s->finished = 1; s->v[SPEED] = 0;
            if (!s->frozen[0]) s->v[ROLL] = 0;
            if (!s->frozen[1]) s->v[PITCH] = 0;
        }
    }
}
static int send_text(Socket sock, const char *text) {
    size_t sent = 0, length = strlen(text);
    while (sent < length) {
        int n = (int)send(sock, text + sent, (int)(length - sent), 0);
        if (n <= 0) { fprintf(stderr, "Server send failed.\n"); return 0; }
        sent += (size_t)n;
    }
    return 1;
}
static int publish(Socket sock, const Simulation *s) {
    char line[512];
    snprintf(line, sizeof(line), "UPDATE %.17g %.17g %.17g %.17g %.17g %.17g %.17g\n",
             s->v[0], s->v[1], s->v[2], s->v[3], s->v[4], s->v[5], s->v[6]);
    return send_text(sock, line);
}
/* The server bundles COMMAND with STATE. Apply only the commanded field from
   that snapshot, avoiding replacement of unrelated, newer local motion. */
static int message(Socket sock, Simulation *s, const char *line) {
    if (!strcmp(line, "OK HELLO SIMULATOR")) {
        s->registered = 1;
        return send_text(sock, "GET\n");
    }
    if (!strncmp(line, "ERR ", 4)) { fprintf(stderr, "Server rejected request: %s\n", line); return 0; }
    if (!strncmp(line, "COMMAND ", 8)) {
        if (strlen(line + 8) >= sizeof(s->pending_command) || s->awaiting_command_state) return 0;
        strcpy(s->pending_command, line + 8);
        s->awaiting_command_state = 1;
    } else if (!strncmp(line, "STATE ", 6)) {
        unsigned long long revision;
        double v[7];
        int flags[3];
        if (sscanf(line, "STATE %llu %lf %lf %lf %lf %lf %lf %lf %d %d %d",
                   &revision, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6],
                   &flags[0], &flags[1], &flags[2]) != 11) return 0;
        if (!s->initialized) {
            int i;
            if (!s->registered) return 0;
            /* Route owns initial position; preserve existing server freeze state. */
            for (i = 0; i < 3; ++i) { s->frozen[i] = flags[i]; if (flags[i]) s->v[i + 4] = v[i + 4]; }
            s->initialized = 1;
        }
        if (s->awaiting_command_state) {
            char operation[16], field[32]; double value;
            const char *names[] = {"longitude", "latitude", "altitude", "airspeed", "roll", "pitch", "yaw"};
            int i;
            if (sscanf(s->pending_command, "%15s %31s %lf", operation, field, &value) != 3) return 0;
            for (i = 0; i < 7 && strcmp(field, names[i]); ++i) {}
            if (!strcmp(operation, "SET") && i < 4) {
                s->v[i] = value;
                if (i == ALT) s->altitude_override = 1;
                if (i == SPEED) s->speed_override = 1;
                if ((i == LON || i == LAT) && s->finished) { s->finished = 0; --s->target; }
            } else if (!strcmp(operation, "FREEZE") && i >= 4 && i < 7) {
                s->frozen[i - 4] = flags[i - 4];
                if (flags[i - 4]) s->v[i] = v[i];
            } else return 0;
            snprintf(s->history[s->command_count % 8], 128, "%s", s->pending_command);
            ++s->command_count; ++s->interval_count;
            s->awaiting_command_state = 0;
        }
    }
    return 1;
}
static void display(Simulation *s, const Route *route, double elapsed) {
    unsigned long n, first = s->command_count > 8 ? s->command_count - 8 : 0;
    printf("\n[t=%.1fs] %s waypoint=%zu/%zu\n", elapsed, s->finished ? "COMPLETE (holding)" : "FLYING",
           s->finished ? route->count : s->target + 1, route->count);
    printf("longitude=%.7f latitude=%.7f altitude=%.2f m airspeed=%.2f m/s\n", s->v[0], s->v[1], s->v[2], s->v[3]);
    printf("roll=%.3f pitch=%.3f yaw=%.3f deg | freeze=%d/%d/%d\n", s->v[4], s->v[5], s->v[6], s->frozen[0], s->frozen[1], s->frozen[2]);
    printf("Commands: total=%lu since_last_print=%lu; recent (up to 8):\n", s->command_count, s->interval_count);
    if (!s->command_count) puts("  (none)");
    for (n = first; n < s->command_count; ++n) printf("  #%lu %s\n", n + 1, s->history[n % 8]);
    s->interval_count = 0;
    fflush(stdout);
}
int main(int argc, char **argv) {
    const char *filename = argc > 1 ? argv[1] : "routes/seattle_short.json";
    const char *host = argc > 2 ? argv[2] : "127.0.0.1";
    long port = 5000; double duration = 0;
    Route route; Simulation sim = {0};
    Socket sock; struct sockaddr_in endpoint;
    char buffer[2048], pending[4097], *end;
    size_t used = 0;
    double start, last, next_print, next_publish, distance, bearing;
    int result = 0;
    if (argc > 5) goto usage;
    if (argc > 3) { port = strtol(argv[3], &end, 10); if (!*argv[3] || *end || port < 1 || port > 65535) goto usage; }
    if (argc > 4) { duration = strtod(argv[4], &end); if (!*argv[4] || *end || !isfinite(duration) || duration <= 0) goto usage; }
    if (!load_route(filename, &route)) return 1;
    sim.v[LON] = route.points[0].longitude; sim.v[LAT] = route.points[0].latitude;
    sim.v[ALT] = route.points[0].altitude; sim.v[SPEED] = route.points[0].airspeed; sim.target = 1;
    direction(sim.v[LON], sim.v[LAT], &route.points[1], &distance, &bearing); sim.v[YAW] = bearing;
#ifdef _WIN32
    { WSADATA data; if (WSAStartup(MAKEWORD(2, 2), &data)) return 1; }
#else
    signal(SIGPIPE, SIG_IGN);
#endif
    signal(SIGINT, stop); signal(SIGTERM, stop);
    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    memset(&endpoint, 0, sizeof(endpoint)); endpoint.sin_family = AF_INET; endpoint.sin_port = htons((unsigned short)port);
    if (sock == BAD_SOCKET || inet_pton(AF_INET, host, &endpoint.sin_addr) != 1 ||
        connect(sock, (struct sockaddr *)&endpoint, sizeof(endpoint))) {
        fprintf(stderr, "Cannot connect to server %s:%ld. Start the server first.\n", host, port);
        if (sock != BAD_SOCKET) close_socket(sock);
        return 1;
    }
    /* Bound blocking sends; receive readiness is handled by select below. */
#ifdef _WIN32
    { DWORD timeout = 2000; setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout)); }
#else
    { struct timeval timeout = {2, 0}; setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)); }
#endif
    printf("Route: %s (%zu waypoints). Server: %s:%ld. Ctrl+C to stop.\n", filename, route.count, host, port);
    fflush(stdout);
    if (!send_text(sock, "HELLO SIMULATOR\n")) { result = 1; goto cleanup; }
    start = last = now_seconds(); next_print = start + 1; next_publish = start;
    while (running) {
        fd_set reads; struct timeval timeout = {0, 10000};
        double now;
        int ready;
        FD_ZERO(&reads); FD_SET(sock, &reads);
        ready = select((int)(sock + 1), &reads, NULL, NULL, &timeout);
        if (ready < 0) { if (running) result = 1; break; }
        if (ready > 0) {
            int count = (int)recv(sock, buffer, sizeof(buffer), 0), i;
            if (count <= 0) { fprintf(stderr, "Server disconnected.\n"); result = 1; break; }
            for (i = 0; i < count; ++i) {
                if (buffer[i] == '\n') {
                    if (used && pending[used - 1] == '\r') --used;
                    pending[used] = 0;
                    if (!message(sock, &sim, pending)) { result = 1; goto cleanup; }
                    used = 0;
                } else {
                    if (used == sizeof(pending) - 1) { fprintf(stderr, "Server line too long.\n"); result = 1; goto cleanup; }
                    pending[used++] = buffer[i];
                }
            }
        }
        now = now_seconds();
        if (!sim.initialized && now - start > 5) { fprintf(stderr, "Server handshake timed out.\n"); result = 1; break; }
        /* No motion/telemetry until the full handshake and command pair finish. */
        if (sim.initialized && !sim.awaiting_command_state) {
            step(&sim, &route, fmin(now - last, 0.1));
            if (now >= next_publish) {
                if (!publish(sock, &sim)) { result = 1; break; }
                next_publish = now + 0.05; /* telemetry at approximately 20 Hz */
            }
            if (now >= next_print) { display(&sim, &route, now - start); next_print += 1; }
        }
        last = now;
        if (duration > 0 && now - start >= duration) break;
    }
cleanup:
    close_socket(sock);
#ifdef _WIN32
    WSACleanup();
#endif
    return result;
usage:
    fprintf(stderr, "Usage: flight_simulator [route.json] [server-IPv4] [port] [run-seconds]\n");
    return 1;
}
