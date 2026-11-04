import collections, os, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import compare as C

CSV = os.path.join(HERE, '..', '..', 'csv', 'door_short_open_hex.csv')

def board_log(path, drop=None, flip=None, lat_offset=3.0):
    msgs = C.read_logic(CSV)
    with open(path, 'w') as f:
        f.write('# synthetic board log\n')
        t = 1000000
        for i, (slot, body, lat) in enumerate(msgs):
            t += 17800
            if i == drop: continue
            if i == flip:
                body = body[:-3] + (body[-3] ^ 0x01,) + body[-2:]
            l = '-' if lat is None else f'{lat + lat_offset:.0f}'
            f.write(f'M {t} {slot:02X} {l} {"-" if not body else 1} {" ".join(f"{x:02X}" for x in body)} | text\n')
            if i % 20 == 0: f.write(f'S {t} slots=1\n')

def unique_indices():
    msgs = C.read_logic(CSV)
    cnt = collections.Counter(m[:2] for m in msgs)
    return [i for i, m in enumerate(msgs) if cnt[m[:2]] == 1 and len(m[1]) > 6]

def run(log):
    return subprocess.run([sys.executable, os.path.join(HERE, 'compare.py'), log, CSV], capture_output=True, text=True)

def test_clean_run(tmp_path):
    log = str(tmp_path / 'b.log'); board_log(log)
    p = run(log)
    assert p.returncode == 0, p.stdout
    r = C.compare(C.read_board(log), C.read_logic(CSV))
    assert not r['missing'] and not r['extra'] and not r['mismatch']
    assert all(abs(v[2] - 3.0) < 1.0 for v in r['lat'].values())

def test_drop_and_changed_byte(tmp_path):
    u = unique_indices()
    assert len(u) >= 2
    log = str(tmp_path / 'b.log'); board_log(log, drop=u[0], flip=u[1])
    r = C.compare(C.read_board(log), C.read_logic(CSV))
    msgs = C.read_logic(CSV)
    assert [m for m in r['missing']] == [msgs[u[0]][:2]]
    assert len(r['mismatch']) == 1 and r['mismatch'][0][0] == msgs[u[1]][:2]
    assert not r['extra']
    p = run(log)
    assert p.returncode == 1 and 'missing' in p.stdout

def test_latency_threshold_fails(tmp_path):
    log = str(tmp_path / 'b.log'); board_log(log, lat_offset=30.0)
    assert run(log).returncode == 1

def test_filtered_log_is_not_missing(tmp_path):
    log = str(tmp_path / 'b.log'); board_log(log)
    def body(l): return tuple(int(x, 16) for x in l[2:].split('|')[0].split()[4:])
    kept = [l for l in open(log) if not l.startswith('M ') or (body(l) and not C.is_idle(body(l)))]
    open(log, 'w').writelines(kept)
    assert run(log).returncode == 0

def test_board_messages_outside_window_are_ignored(tmp_path):
    log = str(tmp_path / 'b.log'); board_log(log)
    extra = 'M 1 0B 666 1 FF 0B 30 00 02 00 00 5D 04 | x\n'
    lines = open(log).readlines()
    open(log, 'w').writelines([extra] + lines + [extra])   # board started earlier and ran longer than the capture
    assert run(log).returncode == 0
