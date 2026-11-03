// A small, dependency-free TCP server. Each message is one ASCII line.
// See README.md for units, commands, and the simulator integration contract.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using Socket = SOCKET;
constexpr Socket invalidSocket = INVALID_SOCKET;
void closeSocket(Socket s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <signal.h>
using Socket = int;
constexpr Socket invalidSocket = -1;
void closeSocket(Socket s) { close(s); }
#endif

// The index order is also the order used by STATE and UPDATE messages.
enum Field { Longitude, Latitude, Altitude, Airspeed, Roll, Pitch, Yaw };
const std::array<std::string, 7> fieldNames = {
    "longitude", "latitude", "altitude", "airspeed", "roll", "pitch", "yaw"};

struct Client {
    explicit Client(Socket socket) : socket(socket) {}
    Socket socket;
    std::mutex outputMutex; // Prevent replies and broadcasts from interleaving.
    std::string role;       // Protected by Server::stateMutex.

    bool sendLine(const std::string& line) {
        std::lock_guard<std::mutex> lock(outputMutex);
        if (socket == invalidSocket) return false;
        const std::string message = line + "\n";
        size_t sent = 0;
        while (sent < message.size()) {
            int count = static_cast<int>(send(socket, message.data() + sent,
                static_cast<int>(message.size() - sent), 0));
            if (count <= 0) return false;
            sent += static_cast<size_t>(count);
        }
        // Log only after the complete message has been written to the socket.
        // One shared mutex keeps different client workers' console lines intact.
        // Forwarded commands contain two protocol lines; label each separately.
        static std::mutex consoleMutex;
        std::lock_guard<std::mutex> consoleLock(consoleMutex);
        std::istringstream lines(line);
        for (std::string entry; std::getline(lines, entry);)
            std::cout << "[SENT socket=" << socket << "] " << entry << '\n';
        std::cout.flush();
        return true;
    }

    void close() {
        std::lock_guard<std::mutex> lock(outputMutex);
        closeSocket(socket);
        socket = invalidSocket;
    }
};

class Server {
    std::mutex stateMutex;
    std::array<double, 7> state{};
    std::array<bool, 3> frozen{};
    std::vector<std::shared_ptr<Client>> clients;
    uint64_t revision = 0;

    // Caller holds stateMutex, giving GET a consistent snapshot.
    std::string snapshot() const {
        std::ostringstream out;
        out << std::setprecision(17) << "STATE " << revision;
        for (double value : state) out << ' ' << value;
        for (bool value : frozen) out << ' ' << (value ? 1 : 0);
        return out.str();
    }

    static bool valid(const std::array<double, 7>& values) {
        for (double value : values) if (!std::isfinite(value)) return false;
        return values[Longitude] >= -180 && values[Longitude] <= 180 &&
            values[Latitude] >= -90 && values[Latitude] <= 90 &&
            values[Airspeed] >= 0 && values[Roll] >= -180 && values[Roll] <= 180 &&
            values[Pitch] >= -90 && values[Pitch] <= 90 &&
            values[Yaw] >= 0 && values[Yaw] < 360;
    }

public:
    void add(const std::shared_ptr<Client>& client) {
        std::lock_guard<std::mutex> lock(stateMutex);
        clients.push_back(client);
    }

    void remove(const std::shared_ptr<Client>& client) {
        std::lock_guard<std::mutex> lock(stateMutex);
        clients.erase(std::remove(clients.begin(), clients.end(), client), clients.end());
    }

    void process(const std::shared_ptr<Client>& client, const std::string& line) {
        std::istringstream input(line);
        std::vector<std::string> tokens;
        for (std::string token; input >> token;) tokens.push_back(token);
        if (tokens.empty()) { client->sendLine("ERR empty_command"); return; }

        std::string reply;
        std::string event;
        std::vector<std::shared_ptr<Client>> simulators;
        {
            // All validation and state mutations happen atomically.
            std::lock_guard<std::mutex> lock(stateMutex);
            if (tokens[0] == "HELLO" && tokens.size() == 2 &&
                (tokens[1] == "CONTROL" || tokens[1] == "SIMULATOR")) {
                if (!client->role.empty()) reply = "ERR already_registered";
                else {
                    bool occupied = false;
                    for (const auto& other : clients)
                        if (other->role == tokens[1]) occupied = true;
                    if (occupied) reply = "ERR role_in_use";
                    else { client->role = tokens[1]; reply = "OK HELLO " + client->role; }
                }
            } else if (client->role.empty()) reply = "ERR send_HELLO_first";
            else if (tokens[0] == "GET" && tokens.size() == 1) reply = snapshot();
            else if (tokens[0] == "SET" && tokens.size() == 3 && client->role == "CONTROL") {
                auto found = std::find(fieldNames.begin(), fieldNames.begin() + 4, tokens[1]);
                auto candidate = state;
                try {
                    size_t used = 0;
                    double value = std::stod(tokens[2], &used);
                    if (found == fieldNames.begin() + 4 || used != tokens[2].size())
                        reply = "ERR invalid_field_or_number";
                    else {
                        candidate[static_cast<size_t>(found - fieldNames.begin())] = value;
                        if (!valid(candidate)) reply = "ERR out_of_range";
                        else { state = candidate; ++revision; event = "COMMAND " + line; reply = "OK " + std::to_string(revision); }
                    }
                } catch (...) { reply = "ERR invalid_number"; }
            } else if (tokens[0] == "FREEZE" && tokens.size() == 3 && client->role == "CONTROL") {
                auto found = std::find(fieldNames.begin() + 4, fieldNames.end(), tokens[1]);
                if (found == fieldNames.end() || (tokens[2] != "0" && tokens[2] != "1"))
                    reply = "ERR expected_FREEZE_axis_0_or_1";
                else {
                    frozen[static_cast<size_t>(found - fieldNames.begin() - 4)] = tokens[2] == "1";
                    ++revision;
                    event = "COMMAND " + line;
                    reply = "OK " + std::to_string(revision);
                }
            } else if (tokens[0] == "UPDATE" && tokens.size() == 8 && client->role == "SIMULATOR") {
                std::array<double, 7> candidate{};
                bool parsed = true;
                try {
                    for (size_t i = 0; i < candidate.size(); ++i) {
                        size_t used = 0;
                        candidate[i] = std::stod(tokens[i + 1], &used);
                        if (used != tokens[i + 1].size()) parsed = false;
                    }
                } catch (...) { parsed = false; }
                if (!parsed || !valid(candidate)) reply = "ERR invalid_telemetry";
                else {
                    // Freeze captures the existing angle; other values still update.
                    for (size_t i = 0; i < frozen.size(); ++i)
                        if (frozen[i]) candidate[i + 4] = state[i + 4];
                    state = candidate;
                    ++revision;
                    reply = "OK " + std::to_string(revision);
                }
            } else reply = "ERR unknown_command_or_wrong_role";

            if (!event.empty()) {
                for (const auto& other : clients)
                    if (other->role == "SIMULATOR") simulators.push_back(other);
                // Include the effective state and freeze flags for the simulator.
                event += "\n" + snapshot();
            }
        }
        // Never hold the state lock while waiting on a network write.
        client->sendLine(reply);
        for (const auto& simulator : simulators) simulator->sendLine(event);
    }
};

void serveClient(Server& server, const std::shared_ptr<Client>& client) {
    client->sendLine("WELCOME aircraft_server_v1");
    std::string pending;
    char buffer[1024];
    bool running = true;
    while (running) {
        int count = recv(client->socket, buffer, sizeof(buffer), 0);
        if (count <= 0) break;
        for (int i = 0; i < count; ++i) {
            if (buffer[i] == '\n') {
                if (!pending.empty() && pending.back() == '\r') pending.pop_back();
                server.process(client, pending);
                pending.clear();
            } else {
                pending += buffer[i];
                if (pending.size() > 4096) {
                    client->sendLine("ERR line_too_long"); running = false; break;
                }
            }
        }
    }
    server.remove(client);
    client->close();
}

int main(int argc, char** argv) {
    // Loopback is the default. Pass a LAN interface address to allow remote clients.
    const std::string address = argc > 1 ? argv[1] : "127.0.0.1";
    int port = 5000;
    try {
        if (argc > 3) throw std::runtime_error("arguments");
        if (argc > 2) {
            size_t used = 0;
            port = std::stoi(argv[2], &used);
            if (used != std::string(argv[2]).size()) throw std::runtime_error("port");
        }
        if (port < 1 || port > 65535) throw std::runtime_error("port");
    } catch (...) {
        std::cerr << "Usage: aircraft_server [IPv4-address] [port 1..65535]\n"; return 1;
    }
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) { std::cerr << "WSAStartup failed\n"; return 1; }
#else
    signal(SIGPIPE, SIG_IGN); // A disconnected peer must not terminate the server.
#endif
    Socket listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(static_cast<unsigned short>(port));
    if (listener == invalidSocket || inet_pton(AF_INET, address.c_str(), &endpoint.sin_addr) != 1 ||
        bind(listener, reinterpret_cast<sockaddr*>(&endpoint), sizeof(endpoint)) != 0 ||
        listen(listener, 8) != 0) {
        std::cerr << "Cannot listen on " << address << ':' << port << " (check address/port availability)\n";
        if (listener != invalidSocket) closeSocket(listener);
        return 1;
    }
    std::cout << "Aircraft server listening on " << address << ':' << port << std::endl;
    // Server lives until process exit; detached workers reference this shared owner.
    auto server = std::make_shared<Server>();
    for (;;) {
        Socket socket = accept(listener, nullptr, nullptr);
        if (socket == invalidSocket) { std::cerr << "accept failed\n"; continue; }
        auto client = std::make_shared<Client>(socket);
        server->add(client);
        std::thread([server, client] { serveClient(*server, client); }).detach();
    }
}
