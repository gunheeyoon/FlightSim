# C flight simulator client

This C11 console application reads a JSON route at startup, follows its waypoints, sends the seven aircraft values to the existing server at approximately 20 Hz, and prints a status block once per second. It reads server messages continuously and displays the last eight control commands, their sequence numbers, and the total received count. No C++ code or third-party libraries are used by this client.

## Build and run on Windows

From the repository root, with MinGW GCC on PATH:

```powershell
gcc -std=c11 -Wall -Wextra -Wpedantic simulator/main.c simulator/route.c -lws2_32 -lm -o flight_simulator.exe
```

Start the server in one terminal:

```powershell
.\aircraft_server.exe
```

Start the simulator in another terminal:

```powershell
.\flight_simulator.exe
```

The console belongs to the terminal where you run the executable. Keep both terminals open. Ctrl+C stops the client. A third terminal can host the future control station. If the server disconnects, the client exits with an error; it does not reconnect automatically.

CMake now builds both executables:

```powershell
cmake -S . -B build
cmake --build build --config Debug
.\build\Debug\flight_simulator.exe routes/seattle_short.json
```

Visual Studio puts executables under `build/Debug`; single-configuration generators normally put them directly under `build`. CMake copies the route to `build/routes`, so the default route also works when running from the build directory. Relative route filenames are resolved against your current working directory.

On Linux/macOS:

```sh
cc -std=c11 -Wall -Wextra -Wpedantic simulator/main.c simulator/route.c -lm -o flight_simulator
./flight_simulator
```

Optional arguments, in order:

```text
flight_simulator [route.json] [server-IPv4] [port] [run-seconds]
```

For example, this exits after 15 seconds:

```powershell
.\flight_simulator.exe routes/seattle_short.json 127.0.0.1 5000 15
```

## Sample flight path and provenance

The short sample is based on the publicly listed [KSEA → KBFI route](https://flightplandatabase.com/plan/5041678), approximately 9 km between Seattle–Tacoma and Boeing Field. Approximate airport coordinates come from the Seattle entries in this [public airport station table](https://downloads.regulations.gov/EPA-HQ-OAR-2019-0178-0493/attachment_8.pdf): KSEA (47.4444, -122.314) and KBFI (47.53028, -122.301).

The intermediate waypoint, altitudes, and speeds are synthetic test values; this is an adaptation of the public airport-to-airport route, not a recorded flight or operational flight plan. It starts airborne at 400 m MSL, climbs to 700 m, then descends to 400 m. At roughly 100–120 m/s it completes in about 90 seconds. It holds the final position and publishes airspeed zero until stopped, so the control station can keep inspecting it.

## JSON schema

```json
{
  "waypoints": [
    { "longitude": -122.314, "latitude": 47.4444, "altitude": 400, "airspeed": 120 },
    { "longitude": -122.301, "latitude": 47.53028, "altitude": 400, "airspeed": 100 }
  ]
}
```

Each waypoint requires exactly four numeric fields; their order is flexible. Units match the server: degrees, metres MSL, and metres/second. The first waypoint initializes position, altitude, and speed; later waypoints are destinations. The destination's speed is approached at up to 5 m/s per second.

The schema-specific parser rejects malformed JSON, trailing content, unknown or duplicate keys, missing fields, nonfinite values, and invalid ranges. Files must be smaller than 64 KiB, with 2–256 waypoints, longitude in [-180,180], latitude strictly between -89 and 89 degrees, and speed in (0,1000] m/s. JSON comments and extra metadata fields are unsupported. Keys must use the literal ASCII spelling shown above. Adjacent coincident positions are permitted and advance on the next step.

## How state is calculated

- **Longitude/latitude:** move toward the target along a spherical great circle using current speed and elapsed monotonic time.
- **Altitude:** interpolate toward the target altitude according to distance travelled along the leg.
- **Airspeed:** approach the destination waypoint speed. Assume zero wind, so airspeed equals horizontal groundspeed for this testing model.
- **Yaw:** initial bearing to the target, approached at up to 20 degrees/second; degrees clockwise from true north, wrapped into [0,360).
- **Pitch:** `atan2(remaining altitude change, remaining horizontal distance)` in degrees; positive means climbing.
- **Roll:** an illustrative coordinated-turn bank from `atan(speed * yaw_rate / gravity)`, limited to ±30 degrees with a 30 degrees/second roll rate; positive means a right bank.

Yaw/roll/pitch are illustrative attitude outputs. The path follower directly determines position; this is a kinematic networking test, not a flight dynamics or aerodynamic model. Freezing an angle holds that output while waypoint motion continues. Horizontal speed zero stops route progress. At the final waypoint the speed becomes zero and unfrozen roll/pitch become zero; yaw holds its last value.

## Commands received through the server

| Command | Client behavior |
| --- | --- |
| `SET longitude value` | Reposition longitude, then continue toward the current target |
| `SET latitude value` | Reposition latitude, then continue toward the current target |
| `SET altitude value` | Immediately set and hold altitude, overriding all later waypoint altitudes |
| `SET airspeed value` | Immediately set and hold speed, overriding all later waypoint speeds |
| `FREEZE roll 1` | Hold the roll value supplied in the server's command snapshot |
| `FREEZE pitch 1` | Hold the pitch value supplied in the server's command snapshot |
| `FREEZE yaw 1` | Hold the yaw value supplied in the server's command snapshot |
| `FREEZE axis 0` | Resume calculating that angle |

Restart the simulator to clear persistent speed/altitude overrides. A position change after completion reactivates the final leg; set a positive airspeed as well to resume motion. SET positions are immediate, then change again with motion; SET altitude and airspeed persist. Commands received before a simulator connects are not replayed. Startup uses the JSON position/speed/altitude and imports existing freeze flags and held angles from GET.

The client handles partial/multiple TCP lines and dispatches replies by their first token. It pairs each `COMMAND` with the following `STATE`, applies the command before its next movement/publication, and records it in the console history. It imports only the relevant snapshot angle for freezes, preventing unrelated cached state from replacing its locally calculated motion. If more than eight commands arrive, the recent history shows the newest eight and the count still includes every command.

The original server protocol still acknowledges cache updates rather than simulator application. There can briefly be in-flight telemetry from before a command. This client applies received commands locally but does not add application acknowledgments or command IDs to the server protocol.

## Automated checks

```powershell
python tests/simulator_integration.py ./aircraft_server.exe ./flight_simulator.exe
```

The checks use real server/client processes, send commands through a control socket, inspect telemetry and console output, check malformed routes, and exercise route completion. No Python packages are required.
