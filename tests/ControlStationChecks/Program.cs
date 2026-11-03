using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using ControlStation;

// Exercise the GUI's actual networking code against the C++ server and C client.
if (args.Length != 2) throw new ArgumentException("Pass server and simulator executable paths.");
var probe = new TcpListener(IPAddress.Loopback, 0);
probe.Start();
int port = ((IPEndPoint)probe.LocalEndpoint).Port;
probe.Stop();

Process Start(string executable, params string[] arguments)
{
    var info = new ProcessStartInfo(Path.GetFullPath(executable))
    {
        UseShellExecute = false, CreateNoWindow = true,
        RedirectStandardOutput = true, RedirectStandardError = true
    };
    foreach (string argument in arguments) info.ArgumentList.Add(argument);
    var process = Process.Start(info)!;
    // Drain logs: the server logs every response and must not block on a full pipe.
    process.OutputDataReceived += (_, _) => { };
    process.ErrorDataReceived += (_, _) => { };
    process.BeginOutputReadLine(); process.BeginErrorReadLine();
    return process;
}

void Check(bool condition, string message)
{
    if (!condition) throw new Exception(message);
}

using var server = Start(args[0], "127.0.0.1", port.ToString());
Process? simulator = null;
try
{
    // Wait for accept readiness without registering either role.
    for (int attempt = 0; ; attempt++)
    {
        try { using var ready = new TcpClient(); await ready.ConnectAsync("127.0.0.1", port); break; }
        catch (SocketException) when (attempt < 50) { await Task.Delay(40); }
    }
    using var connection = new ServerConnection();
    await connection.ConnectAsync("127.0.0.1", port);
    var initial = await connection.GetStateAsync();
    Check(initial.Values.All(v => v == 0), "Initial state");
    using (var duplicate = new ServerConnection())
    {
        try { await duplicate.ConnectAsync("127.0.0.1", port); throw new Exception("Duplicate role accepted"); }
        catch (ServerRejectedException ex) { Check(ex.Message == "role_in_use", "Duplicate role reason"); }
    }
    foreach (string command in new[] { "SET longitude -122.31", "SET latitude 47.48", "SET altitude 900", "SET airspeed 75" })
        await connection.SendCommandAsync(command);
    var changed = await connection.GetStateAsync();
    Check(changed.Values.Take(4).SequenceEqual(new[] { -122.31, 47.48, 900.0, 75.0 }), "SET values");
    try { await connection.SendCommandAsync("SET latitude 91"); throw new Exception("Invalid latitude accepted"); }
    catch (ServerRejectedException) { }
    Check((await connection.GetStateAsync()).Values[1] == 47.48, "Rejected command corrupted session");
    simulator = Start(args[1], "routes/seattle_short.json", "127.0.0.1", port.ToString());
    await Task.Delay(500);
    var moving = await connection.GetStateAsync();
    Check(moving.Values[1] > 47.4444 && moving.Values[1] < 47.48, "Simulator telemetry");
    await connection.SendCommandAsync("SET altitude 1234");
    await connection.SendCommandAsync("SET airspeed 80");
    foreach (string axis in new[] { "roll", "pitch", "yaw" }) await connection.SendCommandAsync($"FREEZE {axis} 1");
    await Task.Delay(300);
    var frozen = await connection.GetStateAsync();
    Check(frozen.Frozen.All(v => v), "Freeze flags");
    await Task.Delay(300);
    var held = await connection.GetStateAsync();
    Check(held.Values.Skip(4).SequenceEqual(frozen.Values.Skip(4)), "Frozen angles changed");
    Check(held.Values[2] == 1234 && held.Values[3] == 80, "Simulator did not apply SET");
    foreach (string axis in new[] { "roll", "pitch", "yaw" }) await connection.SendCommandAsync($"FREEZE {axis} 0");
    Check((await connection.GetStateAsync()).Frozen.All(v => !v), "Unfreeze flags");
    // Concurrent polling and commands must retain request/reply pairing.
    await Task.WhenAll(Enumerable.Range(0, 12).Select(async i =>
    {
        if (i % 2 == 0) await connection.GetStateAsync();
        else await connection.SendCommandAsync("SET altitude 1234");
    }));
    server.Kill(); await server.WaitForExitAsync();
    try { await connection.GetStateAsync(); throw new Exception("Server disconnect was ignored"); }
    catch (IOException) { }
    Console.WriteLine("PASS: registration, duplicate role, live telemetry, all controls, rejection, serialized requests, disconnect");
}
finally
{
    if (simulator is not null) { if (!simulator.HasExited) simulator.Kill(); simulator.WaitForExit(); simulator.Dispose(); }
    if (!server.HasExited) { server.Kill(); server.WaitForExit(); }
}
