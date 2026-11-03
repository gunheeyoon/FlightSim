"""Real-process client/server tests, using only the Python standard library."""
import json
import math
import pathlib
import socket
import subprocess
import sys
import tempfile
import time


def main():
    server_exe, simulator_exe = [str(pathlib.Path(p).resolve()) for p in sys.argv[1:3]]
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        port = probe.getsockname()[1]
    # The server logs every send; discard logs instead of filling an unread pipe.
    server = subprocess.Popen([server_exe, '127.0.0.1', str(port)], stdout=subprocess.DEVNULL)
    simulator = None
    control = None
    try:
        for _ in range(100):
            try:
                control = socket.create_connection(('127.0.0.1', port), timeout=2)
                break
            except OSError:
                time.sleep(0.02)
        assert control is not None
        stream = control.makefile('r', encoding='ascii')
        assert stream.readline().startswith('WELCOME ')

        def request(command):
            control.sendall((command + '\n').encode())
            return stream.readline().strip()

        def state():
            parts = request('GET').split()
            assert parts[0] == 'STATE'
            return list(map(float, parts[2:9])), list(map(int, parts[9:]))

        def wait_for(predicate, timeout=3):
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                value = state()
                if predicate(*value):
                    return value
                time.sleep(0.04)
            raise AssertionError(f'timed out; last state={value}')

        assert request('HELLO CONTROL') == 'OK HELLO CONTROL'
        simulator = subprocess.Popen([simulator_exe, 'routes/seattle_short.json',
                                      '127.0.0.1', str(port), '6'],
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        first, _ = wait_for(lambda v, f: v[0] < -122 and v[1] > 47)
        time.sleep(0.3)
        later, _ = state()
        assert later[1] > first[1] and later[2] > first[2]
        assert all(math.isfinite(v) for v in later)
        for command in ['SET longitude -122.31', 'SET latitude 47.48',
                        'SET altitude 900', 'SET airspeed 75',
                        'FREEZE roll 1', 'FREEZE pitch 1', 'FREEZE yaw 1']:
            assert request(command).startswith('OK ')
        # Let a console interval show these seven commands before the history
        # intentionally rolls over after the later unfreeze/stop commands.
        time.sleep(1.1)
        held, flags = wait_for(lambda v, f: v[2] == 900 and v[3] == 75 and f == [1, 1, 1])
        time.sleep(0.4)
        after, flags = state()
        assert after[2:4] == [900, 75] and after[4:] == held[4:]
        assert after[1] > 47.48 and after[0] > -122.31
        for axis in ['roll', 'pitch', 'yaw']:
            assert request(f'FREEZE {axis} 0').startswith('OK ')
        wait_for(lambda v, f: f == [0, 0, 0])
        assert request('SET airspeed 0').startswith('OK ')
        # Allow already-in-flight telemetry to drain before observing a stop.
        time.sleep(0.3)
        stopped, _ = state()
        time.sleep(0.3)
        still, _ = state()
        assert stopped[:2] == still[:2] and still[3] == 0
        output, error = simulator.communicate(timeout=8)
        assert simulator.returncode == 0, error
        assert 5 <= output.count('[t=') <= 6, output
        for command in ['SET longitude -122.31', 'SET latitude 47.48',
                        'SET altitude 900', 'SET airspeed 75',
                        'FREEZE roll 1', 'FREEZE pitch 1', 'FREEZE yaw 1',
                        'FREEZE roll 0', 'FREEZE pitch 0', 'FREEZE yaw 0', 'SET airspeed 0']:
            assert command in output, command
        simulator = None
        # A tiny route completes quickly, including a turn and a descent.
        with tempfile.TemporaryDirectory() as directory:
            route_file = pathlib.Path(directory) / 'route.json'
            points = [dict(longitude=-122.0, latitude=47.0, altitude=400, airspeed=120),
                      dict(longitude=-122.0, latitude=47.0002, altitude=410, airspeed=120),
                      dict(longitude=-121.9998, latitude=47.0002, altitude=400, airspeed=120)]
            route_file.write_text(json.dumps({'waypoints': points}))
            time.sleep(0.1)  # Let the old simulator's role be released.
            simulator = subprocess.Popen([simulator_exe, str(route_file), '127.0.0.1', str(port), '2.2'],
                                         stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            output, error = simulator.communicate(timeout=5)
            assert simulator.returncode == 0, error
            assert 'COMPLETE (holding)' in output and 'airspeed=0.00' in output
            final, _ = state()
            assert final[:4] == [-121.9998, 47.0002, 400, 0], final
            simulator = None
            valid = json.dumps({'waypoints': points})
            bad_routes = ['{', valid + ' garbage', '\v' + valid, valid.replace('120', 'NaN', 1),
                          valid.replace('120', '01', 1), valid.replace('120', '1e999', 1),
                          valid.replace('"latitude": 47.0', '"latitude": 91.0', 1),
                          valid.replace('"longitude": -122.0', '"longitude": -122.0, "longitude": 0', 1),
                          '{"waypoints": []}', json.dumps({'waypoints': points[:1]}),
                          valid.replace('"airspeed": 120', '"unknown": 120', 1)]
            for bad in bad_routes:
                route_file.write_text(bad)
                check = subprocess.run([simulator_exe, str(route_file)], capture_output=True, text=True, timeout=3)
                assert check.returncode != 0 and 'Invalid route' in check.stderr, (bad, check.stderr)
        print('PASS: motion, all controls, persistent overrides, freezes, console cadence/history, completion, invalid JSON')
    finally:
        if simulator is not None and simulator.poll() is None:
            simulator.terminate()
            simulator.communicate(timeout=5)
        if control is not None:
            control.close()
        server.terminate()
        server.communicate(timeout=5)


if __name__ == '__main__':
    main()
