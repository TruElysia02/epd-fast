"""Start on-device playback and monitor it; control.json permits a clean stop."""
import argparse
import json
from pathlib import Path
import time
import serial

p = argparse.ArgumentParser()
p.add_argument("--port", required=True, help="Serial port of your XIAO, e.g. COM7 or /dev/ttyACM0")
p.add_argument('--seconds', type=int, help='Omit to play the complete video')
p.add_argument('--log', default='.scratch/bad-apple/playback.json')
a = p.parse_args()
if a.seconds is not None and not 1 <= a.seconds <= 233: p.error('seconds must be 1..233')
control = Path('.scratch/bad-apple/control.json')
control.parent.mkdir(parents=True, exist_ok=True)
control.write_text('{"stop":false}', encoding='utf-8')
s = serial.Serial(); s.port = a.port; s.baudrate = 115200; s.timeout = .2
s.dtr = False; s.rts = False; s.open()
log = []; ended = False; idle = False; cancelled = False

def line():
    value = s.readline().decode('utf-8', errors='replace').strip()
    if value: print(value, flush=True); log.append(value)
    return value

try:
    s.write(b'status\n')
    healthy = False
    end = time.monotonic() + 2
    while time.monotonic() < end:
        value = line()
        if 'version=6' in value and 'fault=0' in value: healthy = True; break
    if not healthy: raise RuntimeError('Healthy video firmware not confirmed')
    s.write((f'apple {a.seconds}\n' if a.seconds else 'apple\n').encode())
    deadline = time.monotonic() + (a.seconds or 233) + 60
    while time.monotonic() < deadline:
        value = line()
        if 'FAULT' in value: raise RuntimeError(value)
        if value.startswith('APPLE end;'): ended = True
        if ended and 'running=0' in value and 'fault=0' in value and 'analog=0' in value:
            idle = True; break
        if json.loads(control.read_text(encoding='utf-8')).get('stop'):
            cancelled = True; break
    if not idle and not cancelled: raise RuntimeError('Playback did not finish by deadline')
finally:
    if not idle:
        s.write(b'settle\n')
        end = time.monotonic() + 4
        while time.monotonic() < end:
            value = line()
            if 'running=0' in value and 'fault=0' in value and 'analog=0' in value: idle = True; break
        if not idle:
            s.write(b'stop\n')
            end = time.monotonic() + 8
            while time.monotonic() < end:
                value = line()
                if 'running=0' in value and 'fault=0' in value and 'analog=0' in value: idle = True; break
    s.close()
    out = Path(a.log); out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps({'seconds': a.seconds, 'completed': ended and not cancelled,
                               'cancelled': cancelled, 'healthy_idle': idle, 'log': log},
                              indent=2, ensure_ascii=False), encoding='utf-8')
if not idle: raise SystemExit('FAIL: healthy idle not confirmed')
print('PLAYBACK CANCELLED' if cancelled else 'PLAYBACK COMPLETE; board idle, analog off', flush=True)
