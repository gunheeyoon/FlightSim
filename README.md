# Aircraft server prototype

The companion [C flight simulator guide](simulator/README.md) describes the JSON route, client build/run steps, console display, and control behavior. CMake builds both programs.

The [C# control station guide](control-station/README.md) explains how to build and run the Windows GUI for live state, Change controls, and Freeze/Unfreeze buttons.

A C++17 TCP server for one control station and one flight simulator. No third-party C++ libraries are required. Windows uses Winsock; Linux/macOS use POSIX sockets. State is stored in memory and starts at zero. Restarting clears it.

## Build and run

With CMake and a C++17 compiler:

```powershell
cmake -S . -B build
cmake --build build --config Debug
```

For Visual Studio generators:

```powershell
.\build\Debug\aircraft_server.exe
```

For MinGW with Ninja or Makefiles, the executable is usually `build/aircraft_server.exe`. You can also build directly on Windows with MinGW:

```powershell
g++ -std=c++17 -Wall -Wextra -Wpedantic -pthread src/main.cpp -lws2_32 -o aircraft_server.exe
.\aircraft_server.exe
```

On Linux/macOS, omit `-lws2_32` and use `-o aircraft_server`.

The default endpoint is `127.0.0.1:5000`.

The server prints every successfully sent protocol line to its console with a
`[SENT socket=...]` prefix identifying the destination connection. This includes
welcome messages, acknowledgments, errors, state replies, and forwarded commands.
Successful send means the bytes were written to TCP; it does not confirm client application.

To use another interface/port:

```powershell
.\aircraft_server.exe 192.168.0.10 5000
```

Use an IPv4 address assigned to your server PC. Remote clients connect to that address; Windows Firewall may need an inbound TCP rule. This prototype has no authentication or encryption: use it on a trusted development network. Stop with Ctrl+C (immediate process termination).

## Aircraft data

| Field | Unit / accepted range |
| --- | --- |
| longitude | degrees, -180 through 180 |
| latitude | degrees, -90 through 90 |
| altitude | metres above mean sea level; any finite value, including negative |
| airspeed | metres/second, zero or greater; simulator and station must agree on the airspeed definition |
| roll | degrees, -180 through 180 |
| pitch | degrees, -90 through 90 |
| yaw | heading in degrees, 0 inclusive to 360 exclusive |

Angles and signs must use the same convention in both clients. The server stores numbers; it does not calculate flight physics. Nonfinite values, malformed numbers, extra arguments, and invalid ranges are rejected without changing state.

## Wire protocol

Messages are case-sensitive ASCII lines terminated by LF (`\n`); CRLF also works. TCP can split or combine messages: clients must buffer bytes until LF, and send complete lines. Maximum line length is 4096 bytes excluding LF (including a possible CR). Overlong lines cause an error and disconnect.

The server sends `WELCOME aircraft_server_v1` on connection. Register a role first:

```text
HELLO CONTROL
HELLO SIMULATOR
```

Each connection chooses exactly one role. Only one connection of each role may register at a time; a duplicate receives `ERR role_in_use`. A rejected connection may retry after the previous role disconnects. Registration returns `OK HELLO CONTROL` or `OK HELLO SIMULATOR`.

### Read current state (either role)

```text
GET
```

Response layout:

```text
STATE revision longitude latitude altitude airspeed roll pitch yaw freeze_roll freeze_pitch freeze_yaw
```

Example:

```text
STATE 4 129.0756 35.1796 1000 120 0 0 90 1 0 0
```

Freeze flags are 0 or 1. The revision increases for each successful SET, FREEZE, or UPDATE, including repeated values. GET is an atomic snapshot. Poll GET at a suitable rate, for example 10–20 Hz for a station display; there is no automatic telemetry subscription in this initial version.

### Control station commands

```text
SET longitude 129.0756
SET latitude 35.1796
SET altitude 1000
SET airspeed 120
FREEZE roll 1
FREEZE pitch 1
FREEZE yaw 1
```

Use `FREEZE roll 0`, `FREEZE pitch 0`, or `FREEZE yaw 0` to unfreeze. Freezing holds the angle at its value when the command is processed. Each axis freezes independently. Incoming telemetry continues to update all other fields. Repeating a freeze command leaves the held angle unchanged.

Successful mutations return `OK revision`. Errors return `ERR reason` and leave state unchanged. SET only accepts longitude, latitude, altitude, and airspeed; use simulator telemetry to provide angles.

### Simulator telemetry and command delivery

The simulator sends all seven numbers in this order:

```text
UPDATE 129.0756 35.1796 1000 120 2 3 90
```

The server validates the entire update, replaces its stored values atomically, preserves frozen angles, and replies `OK revision`.

When a control command succeeds, a connected simulator receives two consecutive lines:

```text
COMMAND SET altitude 1500
STATE 12 129.0756 35.1796 1500 120 2 3 90 0 0 0
```

Or:

```text
COMMAND FREEZE roll 1
STATE 13 129.0756 35.1796 1500 120 2 3 90 1 0 0
```

These messages can arrive between telemetry replies. The simulator must continuously read and dispatch lines by their first token, rather than assuming the next line is always an UPDATE reply. The STATE following COMMAND describes the state when that command was processed. Obtain a fresh snapshot with GET after registering or reconnecting; commands are not queued for disconnected simulators.

## State ownership in this prototype

`OK` confirms that the **server cache changed**, not that a simulator applied a command. SET takes effect immediately in the cache even when no simulator is connected. The next UPDATE can replace longitude, latitude, altitude, and airspeed again. A connected simulator must queue COMMAND messages, apply position/speed changes at its simulation update boundary, implement frozen axes in its physics, and then publish the resulting telemetry. Server-side freeze preserves displayed angles but does not stop the simulator's internal physics by itself.

This makes the server independently testable. Before using it for an authoritative simulation, add command IDs and simulator application acknowledgments, command timeout/retry handling, and telemetry sequencing so old telemetry cannot overwrite a newly commanded position. TCP ordering applies per connection, not across the two clients.

## Code tour

- `Client`: serializes outbound writes and handles partial TCP sends.
- `Server::process`: parses commands, checks roles, validates numbers, and updates the shared state under a mutex.
- `Server::snapshot`: formats a consistent state snapshot.
- `serveClient`: reconstructs lines across TCP reads and cleans up disconnected clients.
- `main`: initializes sockets, binds the address, and starts one worker thread per connection.

Network writes happen outside the shared state lock. This prototype uses blocking sockets; slow peers can delay command delivery. It has no heartbeat, idle timeout, persistent storage, subscriptions, or graceful shutdown. Keep it to the intended two development clients.

## Run the integration checks

Build first, then run (Python standard library only):

```powershell
python tests/integration.py ./aircraft_server.exe
```

For a Visual Studio build, pass `./build/Debug/aircraft_server.exe` instead. The script starts an isolated server on an available local port and stops it after testing commands, validation, roles, telemetry, independent freeze/unfreeze behavior, and TCP line framing.
