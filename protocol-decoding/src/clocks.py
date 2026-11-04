"""Who sends which byte? Fingerprint each sender's UART clock from raw edges (see 05-bus-roles.md).
Usage: python3 src/clocks.py raw-captures/digital.csv  (or `just clocks`)

Every device has its own oscillator, so its bit period differs slightly (here ~0.06 %). Only falling
edges are used: the line is pulled low actively but rises through the pull-up, so rising edges are
late by the RC time and would bias the estimate. Each byte is attributed to a role:
'slot' = the byte right after the BREAK, 'srcNN' = body bytes of a message whose source node is NN."""
import csv, sys, collections, statistics as st

NOM=1/57600

def edges(path):
    """[(t, level_after)] from a Logic 2 raw digital export (column 1 = channel 0)."""
    rows=list(csv.reader(open(path)))[1:]
    return [(float(r[0]),int(r[1])) for r in rows][1:]

def split(E):
    """Messages [{'t': break_time, 'brk': low, 'bytes': [(value, [(dt_falling_edge, n_bits)], [(low_width, n_bits)])]}]."""
    msgs=[];cur=None;i=0
    while i<len(E):
        t,v=E[i]
        if v!=0: i+=1; continue
        low=E[i+1][0]-t if i+1<len(E) else 0
        if low>10*NOM: cur={'t':t,'brk':low,'bytes':[]}; msgs.append(cur); i+=2; continue
        ed=[];k=i
        while k<len(E) and E[k][0]-t<9.6*NOM: ed.append(E[k]); k+=1
        level=lambda x:next((ev for et,ev in reversed(ed) if et<=x),1)
        val=sum(level(t+(b+1.5)*NOM)<<b for b in range(8))
        falls=[(et-t,round((et-t)/NOM)) for et,ev in ed[1:] if ev==0]
        lows=[(b[0]-a[0],round((b[0]-a[0])/NOM)) for a,b in zip(ed,ed[1:]) if a[1]==0]
        if cur is not None: cur['bytes'].append((val,falls,lows))
        i=k
    return msgs

def states(msgs,tol=0.2e-6):
    """Edge-timing state per source node over time: {src: [[state, t_first, t_last, n_msgs]]}.
    'clean' = every falling edge of the body within tol of the master's bit grid, else 'jitter'."""
    T=st.median(dt/n for m in msgs if m['bytes'] for dt,n in m['bytes'][0][1] if n>=4)
    out=collections.defaultdict(list)
    for m in msgs:
        bs=m['bytes']
        if len(bs)<4: continue
        err=max((abs(dt-n*T) for val,falls,lows in bs[1:] for dt,n in falls),default=0)
        s='clean' if err<=tol else 'jitter'
        runs=out[bs[2][0]]
        if not runs or runs[-1][0]!=s: runs.append([s,m['t'],m['t'],0])
        runs[-1][2]=m['t']; runs[-1][3]+=1
    return out

def main(path):
    msgs=split(edges(path))
    per=collections.defaultdict(list); low=collections.defaultdict(list); byval=collections.defaultdict(list)
    for m in msgs:
        bs=m['bytes']
        for idx,(val,falls,lows) in enumerate(bs):
            role='slot' if idx==0 else (f'src{bs[2][0]:02X}' if len(bs)>3 else None)
            if role is None: continue
            for dt,n in falls:
                if n>=4: per[role].append(dt/n*1e6); byval[(val,role)].append(dt/n*1e6)
            low[role]+=[w*1e6 for w,n in lows if n==1]
    brk=[m['brk']*1e6 for m in msgs]
    print(f'BREAK length: n={len(brk)} median {st.median(brk):.2f} us, min {min(brk):.2f}, max {max(brk):.2f}')
    print('bit period from falling edges (us):')
    for k,v in sorted(per.items()):
        print(f'  {k:6s} n={len(v):4d} median {st.median(v):.4f}  sd {st.pstdev(v):.4f}  -> {1e6/st.median(v):.0f} baud')
    print('single-bit low pulse width (us):')
    for k,v in sorted(low.items()): print(f'  {k:6s} n={len(v):4d} median {st.median(v):.3f}  sd {st.pstdev(v):.3f}')
    print('same byte value from different senders (median bit period, us):')
    vals=collections.defaultdict(dict)
    for (val,role),v in byval.items(): vals[val][role]=(len(v),st.median(v))
    for val,d in sorted(vals.items()):
        if 'slot' in d and len(d)>1:
            print(f'  {val:02X}: '+'  '.join(f'{r} {m:.4f} (n={n})' for r,(n,m) in sorted(d.items())))
    print('edge timing per sender over time (clean = all falling edges within 0.2 us of the master grid):')
    for src,runs in sorted(states(msgs).items()):
        print(f'  src{src:02X} '+', '.join(f'{s} {a:.3f}-{b:.3f} s ({n})' for s,a,b,n in runs))

if __name__=='__main__':
    main(sys.argv[1])
