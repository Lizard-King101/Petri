"""Runs the display at 8 ticks/s for a while and summarizes its health.

    soak.py SECONDS LOG.txt

Logs every DISPLAY, STREAM, and STORE line to LOG.txt with a timestamp, then
prints: ticks advanced and whether they ever went backwards, the last
fingerprint, frame and update rates, worst draw and decode times, stream
errors, ESP32 heap, and checkpoint saves. It reconnects if the port drops.
PETRI_PORT overrides the serial port (default /dev/ttyACM0).
"""
import os, re, serial, sys, time
if len(sys.argv) != 3:
    sys.exit(__doc__)
duration, log_path = float(sys.argv[1]), sys.argv[2]
port_name = os.environ.get('PETRI_PORT', '/dev/ttyACM0')
s = serial.Serial(port_name, 115200, timeout=0.2)
time.sleep(3)
s.write(b'>>>')
start = time.time(); buf = b''
rows = {'DISPLAY': [], 'STREAM': [], 'STORE': []}
with open(log_path, 'w') as log:
    while time.time() - start < duration:
        try:
            buf += s.read(4096)
        except serial.SerialException as error:
            log.write(f'{time.time()-start:.1f} SERIAL ERROR {error}\n'); log.flush()
            time.sleep(2); s = serial.Serial(port_name, 115200, timeout=0.2); continue
        *lines, buf = buf.split(b'\n')
        for raw in lines:
            line = raw.decode(errors='replace').strip()
            kind = line.split(' ', 1)[0]
            if kind in rows:
                fields = dict(re.findall(r'(\w+)=([\w/-]+)', line))
                fields['t'] = time.time() - start
                rows[kind].append(fields)
                log.write(f'{fields["t"]:.1f} {line}\n')
        log.flush()
d, st, so = rows['DISPLAY'], rows['STREAM'], rows['STORE']
num = lambda r, k: int(r[k].split('/')[0])
ticks = [num(r, 'tick') for r in d if r.get('tick', '0') != '0']
print(f'duration_s={time.time()-start:.0f} display_lines={len(d)} stream_lines={len(st)} store_lines={len(so)}')
if ticks:
    backwards = sum(1 for a, b in zip(ticks, ticks[1:]) if b < a)
    print(f'first_tick={ticks[0]} last_tick={ticks[-1]} advanced={ticks[-1]-ticks[0]} tick_went_backwards={backwards}')
    print(f'last_fingerprint={d[-1]["fingerprint"]} at_tick={d[-1]["tick"]}')
for key in ('fps', 'esp_tps', 'ups'):
    vals = [num(r, key) for r in d[3:]]
    if vals: print(f'{key} min={min(vals)} max={max(vals)}')
for key in ('max_draw_us', 'max_decode_us', 'ram_gap'):
    vals = [num(r, key) for r in d]
    if vals: print(f'{key} min={min(vals)} max={max(vals)}')
if st:
    for key in ('malformed', 'mismatches', 'link_invalid'):
        print(f'{key} first={st[0][key]} last={st[-1][key]}')
    heaps = [num(r, 'esp_min_heap') for r in st]; free = [num(r, 'esp_heap') for r in st]
    print(f'esp_heap min={min(free)} max={max(free)} esp_min_heap_last={heaps[-1]}')
    updates = [num(r, 'updates') for r in st]
    print(f'stream_updates first={updates[0]} last={updates[-1]} reset_seen={any(b < a for a, b in zip(updates, updates[1:]))}')
if so:
    print(f'saves_ok={so[-1]["saves_ok"]} saves_failed={so[-1]["saves_failed"]} last_sequence={so[-1]["last_sequence"]} last_save_ms={so[-1]["last_save_ms"]} restores={so[-1]["restores"]}')
