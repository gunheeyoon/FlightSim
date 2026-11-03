/* Small strict parser for our route schema, not a general JSON library.
   Keys may appear in any order. Unknown/duplicate keys are rejected. */
#include "route.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *start, *p; } Parser;
static void space(Parser *p) {
    /* JSON permits only these four whitespace characters. */
    while (*p->p == ' ' || *p->p == '\t' || *p->p == '\r' || *p->p == '\n') ++p->p;
}
static int take(Parser *p, char c) {
    space(p);
    if (*p->p != c) return 0;
    ++p->p;
    return 1;
}
static int key(Parser *p, char *out, size_t capacity) {
    size_t n = 0;
    if (!take(p, '"')) return 0;
    while (*p->p && *p->p != '"') {
        /* Schema keys are plain ASCII; escapes are deliberately unsupported. */
        if ((unsigned char)*p->p < 32 || *p->p == '\\' || n + 1 >= capacity) return 0;
        out[n++] = *p->p++;
    }
    out[n] = 0;
    return take(p, '"');
}
static int number(Parser *p, double *value) {
    const char *begin;
    char *end;
    space(p);
    begin = p->p;
    if (*p->p == '-') ++p->p;
    if (*p->p == '0') ++p->p;
    else {
        if (*p->p < '1' || *p->p > '9') return 0;
        while (isdigit((unsigned char)*p->p)) ++p->p;
    }
    if (*p->p == '.') {
        ++p->p;
        if (!isdigit((unsigned char)*p->p)) return 0;
        while (isdigit((unsigned char)*p->p)) ++p->p;
    }
    if (*p->p == 'e' || *p->p == 'E') {
        ++p->p;
        if (*p->p == '+' || *p->p == '-') ++p->p;
        if (!isdigit((unsigned char)*p->p)) return 0;
        while (isdigit((unsigned char)*p->p)) ++p->p;
    }
    *value = strtod(begin, &end);
    return end == p->p && isfinite(*value);
}
static int waypoint(Parser *p, Waypoint *point) {
    const char *names[] = {"longitude", "latitude", "altitude", "airspeed"};
    double values[4] = {0};
    unsigned mask = 0;
    char name[32];
    if (!take(p, '{')) return 0;
    do {
        int index;
        if (!key(p, name, sizeof(name)) || !take(p, ':')) return 0;
        for (index = 0; index < 4 && strcmp(name, names[index]); ++index) {}
        if (index == 4 || (mask & (1u << index)) || !number(p, &values[index])) return 0;
        mask |= 1u << index;
    } while (take(p, ','));
    if (!take(p, '}') || mask != 15 || values[0] < -180 || values[0] > 180 ||
        values[1] <= -89 || values[1] >= 89 || values[3] <= 0 || values[3] > 1000) return 0;
    point->longitude = values[0]; point->latitude = values[1];
    point->altitude = values[2]; point->airspeed = values[3];
    return 1;
}
int load_route(const char *filename, Route *route) {
    char text[65537], name[32];
    size_t bytes;
    Parser p;
    int good = 0;
    FILE *file = fopen(filename, "rb");
    if (!file) { fprintf(stderr, "Cannot open route: %s\n", filename); return 0; }
    bytes = fread(text, 1, sizeof(text) - 1, file);
    if (ferror(file) || !feof(file) || memchr(text, 0, bytes)) {
        fclose(file); fprintf(stderr, "Route is too large or unreadable (limit 64 KiB).\n"); return 0;
    }
    fclose(file); text[bytes] = 0;
    p.start = text; p.p = text; route->count = 0;
    if (!take(&p, '{') || !key(&p, name, sizeof(name)) || strcmp(name, "waypoints") ||
        !take(&p, ':') || !take(&p, '[')) goto done;
    do {
        if (route->count == MAX_WAYPOINTS || !waypoint(&p, &route->points[route->count])) goto done;
        ++route->count;
    } while (take(&p, ','));
    if (!take(&p, ']') || !take(&p, '}')) goto done;
    space(&p);
    good = !*p.p && route->count >= 2;
done:
    if (!good) fprintf(stderr, "Invalid route JSON/schema near byte %zu (need 2..256 waypoints).\n", (size_t)(p.p - p.start));
    return good;
}
