using System.Globalization;

namespace ControlStation;

public sealed class MainForm : Form
{
    private static readonly string[] Fields = ["longitude", "latitude", "altitude", "airspeed", "roll", "pitch", "yaw"];
    private static readonly string[] Units = ["deg", "deg", "m MSL", "m/s", "deg", "deg", "deg"];
    private readonly TextBox host = new() { Text = "127.0.0.1", Width = 165, AccessibleName = "Server address" };
    private readonly NumericUpDown port = new() { Minimum = 1, Maximum = 65535, Value = 5000, Width = 90, AccessibleName = "Server port" };
    private readonly Button connect = new() { Text = "Connect", AutoSize = true };
    private readonly Label connectionStatus = new() { Text = "Disconnected", AutoSize = true, Margin = new Padding(12, 8, 3, 3) };
    private readonly Label updated = new() { Text = "No aircraft state received", AutoSize = true };
    private readonly Label[] values = new Label[7];
    private readonly Button[] freezes = new Button[3];
    private readonly List<Button> commands = [];
    private readonly TextBox log = new() { Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Vertical, Dock = DockStyle.Fill };
    private readonly System.Windows.Forms.Timer poll = new() { Interval = 100 };
    private ServerConnection? connection;
    private AircraftState? state;
    private bool busy;
    private bool polling;

    public MainForm()
    {
        Text = "Aircraft Control Station";
        ClientSize = new Size(850, 720);
        MinimumSize = new Size(780, 700);
        Font = new Font("Segoe UI", 10);
        AutoScaleMode = AutoScaleMode.Dpi;
        StartPosition = FormStartPosition.CenterScreen;

        var root = new TableLayoutPanel { Dock = DockStyle.Fill, Padding = new Padding(18), ColumnCount = 1, RowCount = 6 };
        root.RowStyles.Add(new RowStyle(SizeType.Absolute, 42));
        root.RowStyles.Add(new RowStyle(SizeType.Absolute, 250));
        root.RowStyles.Add(new RowStyle(SizeType.Absolute, 190));
        root.RowStyles.Add(new RowStyle(SizeType.Absolute, 30));
        root.RowStyles.Add(new RowStyle(SizeType.Absolute, 25));
        root.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        Controls.Add(root);

        var bar = new FlowLayoutPanel { Dock = DockStyle.Fill, WrapContents = false };
        bar.Controls.AddRange([Caption("Server"), host, Caption("Port"), port, connect, connectionStatus]);
        root.Controls.Add(bar, 0, 0);

        var aircraft = new GroupBox { Text = "Current aircraft state", Dock = DockStyle.Fill };
        var grid = new TableLayoutPanel { Dock = DockStyle.Fill, Padding = new Padding(12, 8, 12, 5), ColumnCount = 4, RowCount = 8 };
        grid.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 27));
        grid.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 32));
        grid.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 16));
        grid.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 25));
        for (int i = 0; i < 8; i++) grid.RowStyles.Add(new RowStyle(SizeType.Percent, 12.5f));
        grid.Controls.Add(Caption("Parameter"), 0, 0);
        grid.Controls.Add(Caption("Current value"), 1, 0);
        grid.Controls.Add(Caption("Unit"), 2, 0);
        grid.Controls.Add(Caption("Attitude control"), 3, 0);
        for (int i = 0; i < Fields.Length; i++)
        {
            int index = i;
            grid.Controls.Add(Caption(Title(Fields[i])), 0, i + 1);
            values[i] = new Label { Text = "—", AutoSize = true, Anchor = AnchorStyles.Left, AccessibleName = "Current " + Fields[i] };
            grid.Controls.Add(values[i], 1, i + 1);
            grid.Controls.Add(Caption(Units[i]), 2, i + 1);
            if (i >= 4)
            {
                var button = new Button { Text = "Freeze " + Fields[i], Dock = DockStyle.Fill, Margin = new Padding(0, 1, 0, 1), Enabled = false };
                freezes[i - 4] = button;
                commands.Add(button);
                button.Click += async (_, _) =>
                {
                    if (state is null) return;
                    await SendAsync($"FREEZE {Fields[index]} {(state.Frozen[index - 4] ? 0 : 1)}");
                };
                grid.Controls.Add(button, 3, i + 1);
            }
        }
        aircraft.Controls.Add(grid);
        root.Controls.Add(aircraft, 0, 1);

        var changes = new GroupBox { Text = "Change aircraft values", Dock = DockStyle.Fill };
        var changesGrid = new TableLayoutPanel { Dock = DockStyle.Fill, Padding = new Padding(12), ColumnCount = 4, RowCount = 4 };
        changesGrid.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 25));
        changesGrid.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 30));
        changesGrid.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 25));
        changesGrid.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 20));
        string[] hints = ["-180 to 180 deg", "-90 to 90 deg", "metres MSL", "0 or greater m/s"];
        string[] defaults = ["-122.31", "47.48", "900", "75"];
        for (int i = 0; i < 4; i++)
        {
            changesGrid.RowStyles.Add(new RowStyle(SizeType.Percent, 25));
            int index = i;
            var input = new TextBox { Text = defaults[i], Dock = DockStyle.Fill, AccessibleName = "New " + Fields[i] };
            var button = new Button { Text = "Change " + Fields[i], Dock = DockStyle.Fill, Enabled = false };
            commands.Add(button);
            button.Click += async (_, _) =>
            {
                // Use a dot decimal separator regardless of Windows locale.
                if (!double.TryParse(input.Text.Trim(), NumberStyles.Float, CultureInfo.InvariantCulture, out double value) ||
                    !double.IsFinite(value) || (index == 0 && (value < -180 || value > 180)) ||
                    (index == 1 && (value < -90 || value > 90)) || (index == 3 && value < 0))
                {
                    AppendLog($"Invalid {Fields[index]}. Enter a finite number ({hints[index]}); use '.' for decimals.");
                    input.Focus(); input.SelectAll(); return;
                }
                await SendAsync($"SET {Fields[index]} {value.ToString("R", CultureInfo.InvariantCulture)}");
            };
            input.KeyDown += (_, e) => { if (e.KeyCode == Keys.Enter) { e.SuppressKeyPress = true; button.PerformClick(); } };
            changesGrid.Controls.Add(Caption(Title(Fields[i])), 0, i);
            changesGrid.Controls.Add(input, 1, i);
            changesGrid.Controls.Add(Caption(hints[i]), 2, i);
            changesGrid.Controls.Add(button, 3, i);
        }
        changes.Controls.Add(changesGrid);
        root.Controls.Add(changes, 0, 2);
        root.Controls.Add(updated, 0, 3);
        root.Controls.Add(Caption("Command and connection log"), 0, 4);
        root.Controls.Add(log, 0, 5);
        connect.Click += async (_, _) => { if (connection is null) await ConnectAsync(); else Disconnect("Disconnected"); };
        poll.Tick += async (_, _) => await PollAsync();
        FormClosed += (_, _) => { poll.Stop(); poll.Dispose(); connection?.Dispose(); };
    }

    private static string Title(string text) => char.ToUpperInvariant(text[0]) + text[1..];
    private static Label Caption(string text) => new() { Text = text, AutoSize = true, Anchor = AnchorStyles.Left, Margin = new Padding(3, 5, 8, 3) };

    private void SetBusy(bool value)
    {
        busy = value;
        foreach (Button button in commands) button.Enabled = !value && connection is not null && state is not null;
    }

    private void AppendLog(string text)
    {
        // Bound the log so a long test session cannot grow it indefinitely.
        if (log.TextLength > 30000) log.Text = log.Text[^15000..];
        log.AppendText($"[{DateTime.Now:HH:mm:ss}] {text}{Environment.NewLine}");
    }

    private void ShowState(AircraftState snapshot)
    {
        state = snapshot;
        for (int i = 0; i < 7; i++)
        {
            values[i].Text = snapshot.Values[i].ToString(i < 2 ? "F7" : "F3", CultureInfo.InvariantCulture);
            values[i].ForeColor = SystemColors.ControlText;
        }
        for (int i = 0; i < 3; i++)
        {
            freezes[i].Text = (snapshot.Frozen[i] ? "Unfreeze " : "Freeze ") + Fields[i + 4];
            freezes[i].BackColor = snapshot.Frozen[i] ? Color.LightGoldenrodYellow : SystemColors.Control;
        }
        updated.Text = $"Received {DateTime.Now:HH:mm:ss.fff}  |  Server revision {snapshot.Revision}  |  Refresh: 10 Hz";
    }

    private async Task ConnectAsync()
    {
        if (string.IsNullOrWhiteSpace(host.Text)) { AppendLog("Enter the server address."); return; }
        var session = new ServerConnection();
        connection = session;
        connect.Text = "Disconnect"; host.Enabled = port.Enabled = false;
        connectionStatus.Text = "Connecting…";
        SetBusy(true);
        try
        {
            await session.ConnectAsync(host.Text.Trim(), (int)port.Value);
            var snapshot = await session.GetStateAsync();
            if (connection != session || IsDisposed) return;
            ShowState(snapshot); connectionStatus.Text = "Connected";
            AppendLog($"Connected to {host.Text}:{port.Value} as CONTROL.");
            poll.Start();
        }
        catch (Exception ex) { if (connection == session && !IsDisposed) Disconnect("Connection failed: " + ErrorText(ex)); }
        finally { if (connection == session && !IsDisposed) SetBusy(false); }
    }

    private async Task PollAsync()
    {
        var session = connection;
        if (session is null || busy || polling) return;
        // Polls do not disable the buttons: clicks skip that poll interval and
        // wait for its serialized request, keeping the GUI pleasant to use.
        polling = true;
        try
        {
            var snapshot = await session.GetStateAsync();
            if (connection == session && !IsDisposed) ShowState(snapshot);
        }
        catch (Exception ex) { if (connection == session && !IsDisposed) Disconnect("Connection lost: " + ErrorText(ex)); }
        finally { if (connection == session && !IsDisposed) polling = false; }
    }

    private async Task SendAsync(string command)
    {
        var session = connection;
        if (session is null) return;
        SetBusy(true);
        try
        {
            string reply = await session.SendCommandAsync(command);
            var snapshot = await session.GetStateAsync();
            if (connection != session || IsDisposed) return;
            AppendLog($"{command} → {reply} (server accepted)");
            ShowState(snapshot);
        }
        catch (ServerRejectedException ex) { if (connection == session && !IsDisposed) AppendLog("Server rejected command: " + ex.Message); }
        catch (Exception ex) { if (connection == session && !IsDisposed) Disconnect("Connection lost: " + ErrorText(ex)); }
        finally { if (connection == session && !IsDisposed) SetBusy(false); }
    }

    private static string ErrorText(Exception ex) => ex is OperationCanceledException ? "Request timed out." : ex.Message;

    private void Disconnect(string reason)
    {
        poll.Stop();
        polling = false;
        var old = connection; connection = null; old?.Dispose(); state = null;
        connect.Text = "Connect"; host.Enabled = port.Enabled = true;
        connectionStatus.Text = "Disconnected";
        updated.Text = "Disconnected — displayed values are stale";
        foreach (Label value in values) value.ForeColor = SystemColors.GrayText;
        AppendLog(reason);
        SetBusy(false);
    }
}
