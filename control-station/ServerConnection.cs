using System.Globalization;
using System.Net.Sockets;
using System.Text;

namespace ControlStation;

// Immutable snapshots keep the displayed fields and freeze flags consistent.
public sealed record AircraftState(ulong Revision, double[] Values, bool[] Frozen)
{
    public static AircraftState Parse(string line)
    {
        string[] parts = line.Split(' ', StringSplitOptions.RemoveEmptyEntries);
        if (parts.Length != 12 || parts[0] != "STATE")
            throw new IOException("Invalid aircraft state received from the server.");
        ulong revision = ulong.Parse(parts[1], CultureInfo.InvariantCulture);
        double[] values = new double[7];
        bool[] frozen = new bool[3];
        for (int i = 0; i < 7; i++)
        {
            values[i] = double.Parse(parts[i + 2], CultureInfo.InvariantCulture);
            if (!double.IsFinite(values[i])) throw new IOException("Nonfinite aircraft state.");
        }
        for (int i = 0; i < 3; i++)
        {
            if (parts[i + 9] is not ("0" or "1")) throw new IOException("Invalid freeze flag.");
            frozen[i] = parts[i + 9] == "1";
        }
        return new(revision, values, frozen);
    }
}

public sealed class ServerRejectedException(string message) : Exception(message);

// The control protocol is request/reply. Serialize complete exchanges so a
// polling GET can never consume a command's acknowledgment (or vice versa).
public sealed class ServerConnection : IDisposable
{
    private readonly TcpClient tcp = new();
    private readonly SemaphoreSlim exchange = new(1, 1);
    private readonly CancellationTokenSource lifetime = new();
    private StreamReader? reader;
    private StreamWriter? writer;

    private CancellationTokenSource Timeout()
    {
        var timeout = CancellationTokenSource.CreateLinkedTokenSource(lifetime.Token);
        timeout.CancelAfter(TimeSpan.FromSeconds(3));
        return timeout;
    }

    public async Task ConnectAsync(string host, int port)
    {
        using var timeout = Timeout();
        await tcp.ConnectAsync(host, port, timeout.Token);
        tcp.NoDelay = true;
        var stream = tcp.GetStream();
        reader = new StreamReader(stream, Encoding.ASCII, false, 1024, leaveOpen: true);
        writer = new StreamWriter(stream, Encoding.ASCII, 1024, leaveOpen: true) { NewLine = "\n" };
        if (await ReadAsync(timeout.Token) != "WELCOME aircraft_server_v1")
            throw new IOException("Unexpected server greeting.");
        string reply = await RequestAsync("HELLO CONTROL");
        if (reply != "OK HELLO CONTROL") throw new IOException("Unexpected registration reply.");
    }

    private async Task<string> ReadAsync(CancellationToken token)
    {
        // Reading one character at a time is buffered by StreamReader, and
        // enforces a line limit even if a broken server never sends a newline.
        var line = new StringBuilder();
        var character = new char[1];
        while (await reader!.ReadAsync(character.AsMemory(), token) != 0)
        {
            if (character[0] == '\n') return line.ToString().TrimEnd('\r');
            if (line.Length >= 4096) throw new IOException("Server message is too long.");
            line.Append(character[0]);
        }
        throw new IOException("The server closed the connection.");
    }

    public async Task<string> RequestAsync(string command)
    {
        using var timeout = Timeout();
        await exchange.WaitAsync(timeout.Token);
        try
        {
            if (writer is null) throw new IOException("Not connected.");
            await writer.WriteLineAsync(command.AsMemory(), timeout.Token);
            await writer.FlushAsync(timeout.Token);
            string reply = await ReadAsync(timeout.Token);
            if (reply.StartsWith("ERR ", StringComparison.Ordinal))
                throw new ServerRejectedException(reply[4..]);
            return reply;
        }
        finally { exchange.Release(); }
    }

    public async Task<AircraftState> GetStateAsync() => AircraftState.Parse(await RequestAsync("GET"));

    public async Task<string> SendCommandAsync(string command)
    {
        string reply = await RequestAsync(command);
        if (!reply.StartsWith("OK ", StringComparison.Ordinal))
            throw new IOException("Unexpected command response: " + reply);
        return reply;
    }

    public void Dispose()
    {
        // Closing TCP interrupts any in-flight read without blocking the GUI.
        lifetime.Cancel();
        tcp.Dispose();
    }
}
