#!/usr/bin/env python3
"""Compare a board log (M lines, design D6) with a Logic 2 Async Serial hex export of the same moment (design D7).
Usage: python3 tools/compare.py <board.log> <logic.csv>
Both sides are framed as (slot byte, body bytes); the sequences are aligned with difflib, not by time (the clocks differ).
Filter rule: the board filters empty slots and idle replies by default. If the log contains no empty slots (or no idle
replies), the same category is dropped from the analyzer side, so a filtered log is not reported as "missing".
Board messages before the first and after the last matched message (the board log may start earlier and end later than
the capture) are ignored.
Exit code 1 if matched < 99.9 %, any extra message, or a per-source median latency differs by more than 10 us."""
import difflib, os, statistics, sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'src'))
from slots import bursts

MIN_MATCH, MAX_LAT_DIFF_US = 0.999, 10.0

def is_idle(body):
    """`30 00` / `30 01` ack with payload `02 00 00`: dst src 30 fl 02 00 00 crc crc."""
    return len(body) == 9 and body[2] == 0x30 and body[3] in (0, 1) and tuple(body[4:7]) == (2, 0, 0)

def read_board(path):
    """[(slot, body tuple, latency us | None)] from the M lines; S, E and # lines are ignored."""
    out = []
    for line in open(path):
        if not line.startswith('M '): continue
        t = line[2:].split('|')[0].split()
        slot, lat = int(t[1], 16), (None if t[2] == '-' else float(t[2]))
        out.append((slot, tuple(int(x, 16) for x in t[4:]), lat))
    return out

def read_logic(path):
    out = []
    for _, b, ts in bursts(path):
        if not b: continue                                  # glitch read as a break
        lat = (ts[1] - ts[0]) * 1e6 if len(b) > 1 else None
        out.append((b[0], tuple(b[1:]), lat))
    return out

def compare(board, logic):
    drop_empty = not any(not b for _, b, _ in board)
    drop_idle = not any(is_idle(b) for _, b, _ in board)
    logic = [m for m in logic if not ((drop_empty and not m[1]) or (drop_idle and is_idle(m[1])))]
    a, b = [m[:2] for m in logic], [m[:2] for m in board]
    ops = difflib.SequenceMatcher(None, a, b, autojunk=False).get_opcodes()
    r = dict(total=len(a), matched=0, missing=[], extra=[], mismatch=[], lat={})
    pairs = []
    eq = [o for o in ops if o[0] == 'equal']
    jlo, jhi = (eq[0][3], eq[-1][4]) if eq else (0, len(b))   # board messages outside the analyzer window are not extras
    for tag, i1, i2, j1, j2 in ops:
        if tag == 'equal':
            r['matched'] += i2 - i1
            pairs += [(logic[i][2], board[j][2], logic[i][1]) for i, j in zip(range(i1, i2), range(j1, j2))]
        elif tag in ('replace', 'delete', 'insert'):
            k = min(i2 - i1, j2 - j1) if tag == 'replace' else 0
            r['mismatch'] += [(logic[i1 + n][:2], board[j1 + n][:2]) for n in range(k)]
            r['missing'] += [logic[i][:2] for i in range(i1 + k, i2)]
            r['extra'] += [board[j][:2] for j in range(j1 + k, j2) if jlo <= j < jhi]
    per = {}
    for la, lb_, body in pairs:
        if la is not None and lb_ is not None and len(body) > 1: per.setdefault(body[1], ([], []))[0].append(la) or per[body[1]][1].append(lb_)
    for src, (x, y) in per.items():
        r['lat'][src] = (statistics.median(x), statistics.median(y), statistics.median(y) - statistics.median(x), len(x))
    return r

def verdict(r):
    fails = []
    if r['total'] and r['matched'] / r['total'] < MIN_MATCH: fails.append(f'matched {r["matched"]}/{r["total"]} < 99.9 %')
    if r['extra']: fails.append(f'{len(r["extra"])} extra messages')
    for src, (_, _, d, _) in r['lat'].items():
        if abs(d) > MAX_LAT_DIFF_US: fails.append(f'source {src:02X} latency differs by {d:+.1f} us')
    return fails

def fmt(m): return f'{m[0]:02X} ' + ' '.join(f'{x:02X}' for x in m[1])

def main(argv):
    r = compare(read_board(argv[1]), read_logic(argv[2]))
    print(f'analyzer messages {r["total"]}: matched {r["matched"]}, missing {len(r["missing"])}, '
          f'extra {len(r["extra"])}, mismatch {len(r["mismatch"])}')
    for m in r['missing']: print('  missing from board:', fmt(m))
    for m in r['extra']: print('  extra on board:    ', fmt(m))
    for a, b in r['mismatch']: print('  mismatch: analyzer', fmt(a), '| board', fmt(b))
    print('median latency per source (us): analyzer, board, board-analyzer, n')
    for src, (x, y, d, n) in sorted(r['lat'].items()): print(f'  {src:02X}: {x:6.0f} {y:6.0f} {d:+6.1f}  n={n}')
    fails = verdict(r)
    print('FAIL: ' + '; '.join(fails) if fails else 'PASS')
    return 1 if fails else 0

if __name__ == '__main__':
    sys.exit(main(sys.argv))
