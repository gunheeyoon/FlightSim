# C# aircraft control station

A Windows Forms desktop client for the C++ aircraft server and C flight simulator. It targets .NET 9 for Windows, matching the SDK installed in this workspace, and uses no third-party packages. See Microsoft's [Windows Forms overview](https://learn.microsoft.com/en-us/dotnet/desktop/winforms/overview/) for the UI framework.

## Build and run

From the repository root, with the .NET 9 SDK installed:

```powershell
dotnet build control-station/ControlStation.csproj
```

Start the existing programs in separate terminals:

```powershell
.\build\aircraft_server.exe
```

```powershell
.\build\flight_simulator.exe routes/seattle_short.json
```

Then launch the GUI:

```powershell
.\control-station\bin\Debug\net9.0-windows\ControlStation.exe
```

Alternatively run `dotnet run --project control-station/ControlStation.csproj`. For Visual Studio CMake builds, use `build/Debug` for the two native executables. You can also open `ControlStation.csproj` directly in Visual Studio with the .NET desktop development workload.

## Use the window

1. Enter the server address and port (defaults: `127.0.0.1`, `5000`) and click **Connect**.
2. The state panel displays longitude, latitude, altitude, airspeed, roll, and pitch/yaw, refreshing at up to 10 Hz. Units and the last received time are shown. These are server snapshots; the current protocol cannot tell whether a simulator is connected or whether cached telemetry is stale.
3. Enter a value in one of the four change text boxes and click its **Change** button, or press Enter in that text box. GUI Change sends the existing protocol's `SET` command. Use a dot decimal separator. The GUI rejects nonfinite values, out-of-range coordinates, and negative airspeed before sending them.
4. Click **Freeze roll**, **Freeze pitch**, or **Freeze yaw** to hold that angle. Each button changes to **Unfreeze** and receives a yellow background while the server reports that axis frozen. Click again to resume calculating that angle.
5. Read the command log for acceptance or errors. Click **Disconnect** to release the CONTROL role. Closing the window also closes the socket.

Control buttons stay disabled until a successful connection and first state snapshot. Connection failures, duplicate CONTROL clients, and timeouts appear in the log. After a connection loss, displayed values turn grey and are explicitly marked stale. Reconnect manually with Connect.

The current server permits only one control station. The simulator must be running to see changing telemetry and have commands applied to its motion. Without it, the GUI still reads and changes the server cache. `OK` means server acceptance, not simulator application. The simulator holds speed/altitude overrides until restarted; longitude/latitude changes reposition it and then waypoint motion continues. Frozen attitude values do not stop the kinematic waypoint motion. See the [simulator guide](../simulator/README.md) for these behaviors.

## Code guide

- `Program.cs`: STA entry point and application startup.
- `MainForm.cs`: commented UI layout, input validation, polling, button actions, and connection status.
- `ServerConnection.cs`: asynchronous TCP registration, bounded newline framing, invariant numeric parsing, and serialized request/reply exchanges. Each network exchange has a three-second timeout; network operations do not block the UI thread.

Controls use standard Windows widgets and DPI scaling. Resizing expands the log area. Editing a change input is independent of telemetry refresh, so incoming snapshots never overwrite the number you are typing.

## Integration checks

```powershell
dotnet run --project tests/ControlStationChecks -- ./build/aircraft_server.exe ./build/flight_simulator.exe
```

The checks start isolated native processes on an available local port and exercise the same networking class used by the GUI: registration, duplicate role rejection, all SET/FREEZE commands, live telemetry, invalid command rejection, concurrent requests, and server disconnect. No external test packages are needed.
