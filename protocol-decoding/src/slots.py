"""Bus-role analysis: slot schedule, responder latency and node addressing (see 05-bus-roles.md).
Usage: python3 src/slots.py csv/*_hex.csv  (or `just slots`)

Model being checked: BREAK + <slot> is sent by the scheduler; the slot owner then sends
<dst> <src> <svc> <flag> <len> <payload> <crc lo> <crc hi>, with dst = 0x80|node, src = node."""
import csv, sys, collections, statistics as st

def bursts(path):
    """[(t_break, [byte...], [t_byte...])] per BREAK, with per-byte start times (needed for gaps)."""
    out=[];cur=None
    for r in csv.DictReader(open(path)):
        t,err,b=float(r['start_time']),r['error'],int(r['data'],16)
        if err=='framing' and b==0: cur=(t,[],[]);out.append(cur);continue
        if cur: cur[1].append(b);cur[2].append(t)
    return out

def node(a): return 'bcast' if a==0x80 else f'{a&0x7F:02X}'

AWAKE_MAX_US=630    # node 01/0B answers after ~604 us awake, 648-689 us in low-power mode (05-bus-roles.md §2)

def ui_states(path):
    """Node 01/0B power state over time from its answer latency: [[state, t_first, t_last, n, lat_min, lat_max]]."""
    out=[]
    for t,b,ts in bursts(path):
        if len(b)<4 or b[2] not in (0x01,0x0B): continue
        lat=(ts[1]-ts[0])*1e6
        s='awake' if lat<AWAKE_MAX_US else 'low-power'
        if not out or out[-1][0]!=s: out.append([s,t,t,0,lat,lat])
        r=out[-1]; r[2]=t; r[3]+=1; r[4]=min(r[4],lat); r[5]=max(r[5],lat)
    return out

def door_pushes(path):
    """[(t, open)] from node 0B's broadcast of the door switch 14/04."""
    return [(t,bool(b[9])) for t,b,ts in bursts(path)
            if len(b)>9 and b[2]==0x0B and b[3]==0x24 and b[6:9]==[0x14,0x04,0x20]]

def main(paths):
    lat=collections.defaultdict(list); addr=collections.Counter(); svc=collections.Counter()
    slots=collections.Counter(); bare=collections.Counter(); period=[]; inner=[]
    for p in paths:
        bs=bursts(p)
        period+=[(b[0]-a[0])*1e3 for a,b in zip(bs,bs[1:])]
        for t,b,ts in bs:
            if not b: continue                              # power-up glitch read as a break
            slots[b[0]]+=1
            if len(b)==1: bare[b[0]]+=1; continue
            lat[(b[0],b[2])].append((ts[1]-ts[0])*1e6)
            inner+=[(y-x)*1e6 for x,y in zip(ts[1:],ts[2:])]
            if len(b)>=6:
                addr[(b[0],b[1],b[2])]+=1; svc[(b[2],b[3])]+=1
    print('slot byte   total  bare(no answer)')
    for s,n in sorted(slots.items()): print(f'  {s:02X}      {n:6d}  {bare[s]:6d}')
    print('\nslot -> dst/src (node numbers: dst & 0x7F, 80 = broadcast)')
    for (s,d,r),n in sorted(addr.items()): print(f'  slot {s:02X}: dst {d:02X} (node {node(d)})  src {r:02X}  n={n}')
    print('\nresponder latency, end of slot byte start -> first body byte start (us)')
    for (s,r),v in sorted(lat.items()):
        print(f'  slot {s:02X} src {r:02X}: n={len(v):4d} median {st.median(v):5.0f}  sd {st.pstdev(v):5.1f}  min {min(v):4.0f} max {max(v):4.0f}')
    print(f'\nbyte spacing inside a body: median {st.median(inner):.1f} us (10 bits at 57600 = 173.6 us)')
    h=collections.Counter(round(x) for x in period if x<100)
    print('break-to-break period (ms):',', '.join(f'{k} ms x{n}' for k,n in sorted(h.items())))
    print('\nservice codes by source node')
    for (r,c),n in sorted(svc.items()): print(f'  src {r:02X} svc {c:02X}  n={n}')
    print('\nnode 01/0B power state (from its answer latency) and door pushes, per capture')
    for p in paths:
        door=', '.join(f'{t:.2f} s {"open" if o else "closed"}' for t,o in door_pushes(p))
        print(f'  {p.split("/")[-1]}'+(f'  door: {door}' if door else ''))
        for s,a,b,n,lo,hi in ui_states(p): print(f'    {s:9s} {a:7.3f}-{b:7.3f} s  n={n:3d}  latency {lo:.0f}-{hi:.0f} us')

if __name__=='__main__':
    main(sys.argv[1:])
