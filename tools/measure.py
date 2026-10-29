"""Bounded hardware comparison. Always stop and confirm idle before exiting."""
import argparse
import json
from pathlib import Path
import re
import statistics
import time
import serial

p = argparse.ArgumentParser()
p.add_argument("--port", required=True, help="Serial port of your XIAO, e.g. COM7 or /dev/ttyACM0")
p.add_argument('--end', choices=['normal', 'fast'])
p.add_argument('--spi', type=int, choices=[4, 10, 20])
p.add_argument('--speed', type=int)
p.add_argument('--mode', choices=['budget', 'net'], default='budget')
p.add_argument('--seconds', type=float, default=7)
p.add_argument('--min-hz', type=float, default=20)
p.add_argument('--output', required=True)
a = p.parse_args()
if not 3 <= a.seconds <= 30: p.error('seconds must be 3..30')
s = serial.Serial()
s.port = a.port; s.baudrate = 115200; s.timeout = .1
s.dtr = False; s.rts = False; s.open()
lines = []

def read_for(seconds):
    result = []
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        line = s.readline().decode('utf-8', errors='replace').strip()
        if line:
            print(line, flush=True); lines.append(line); result.append(line)
    return result

def send(command, seconds=.5):
    print('COMMAND', command, flush=True)
    s.write((command + '\n').encode())
    return read_for(seconds)

stopped = False
try:
    state = send('status')
    if not any('fault=0' in x for x in state): raise RuntimeError('No healthy status')
    send('fps 0')
    if a.spi: send(f'spi {a.spi}', 6)
    if a.end: send(f'end {a.end}', 6)
    if a.speed: send(f'speed {a.speed}', 6)
    # Diagnostic builds retain a 3s guard between full and fast preparation.
    samples = send(a.mode, a.seconds + 5.5)
    if any('FAULT' in x for x in samples): raise RuntimeError('Device fault')
finally:
    send('stop', 8)
    idle = send('status', .5)
    stopped = any('running=0' in x and 'fault=0' in x and 'analog=0' in x for x in idle)
    s.close()
    metrics = []
    for line in lines:
        if line.startswith('MEASURE '):
            metrics.append({key: float(value) for key, value in
                            re.findall(r'(scan_hz|plan_us|upload_us|busy_us|render_us|commit_us)=([\d.]+)', line)})
    report = {'arguments': vars(a), 'stopped_healthy': stopped,
              'metrics': metrics, 'log': lines}
    dest = Path(a.output); dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding='utf-8')
if not stopped: raise SystemExit('FAIL: device stop not confirmed')
if len(metrics) < 2: raise SystemExit('FAIL: fewer than two measurement intervals')
hz = statistics.mean(x['scan_hz'] for x in metrics)
print(f'MEAN scan_hz={hz:.2f}; stopped_healthy={stopped}')
if hz < a.min_hz: raise SystemExit(f'FAIL: scan rate {hz:.2f} < {a.min_hz}')
print('PASS')
