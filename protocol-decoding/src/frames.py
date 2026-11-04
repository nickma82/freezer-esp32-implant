"""Split a Logic 2 data-table export (hex radix, Async Serial analyzer on the bus wire) into messages.
A message starts with the BREAK, which the analyzer reports as a 0x00 byte with a framing error."""
import csv,sys

GAP_S=0.002   # idle gap that separates two messages

def frames(path,gap=GAP_S):
    """List of bursts; each burst is a list of (hex_str, framing_error, start_time_s)."""
    rows=list(csv.DictReader(open(path)))
    prev=None;out=[];cur=[]
    for x in rows:
        t=float(x['start_time'])
        if prev is not None and t-prev>gap:
            out.append(cur);cur=[]
        cur.append((x['data'][2:],bool(x['error']),t));prev=t+float(x['duration'])
    if cur: out.append(cur)
    return out

def messages(path,gap=GAP_S):
    """[(start_time_s, bytes after the break)] for every burst that begins with a break."""
    res=[]
    for f in frames(path,gap):
        if f[0][0]=='00' and f[0][1] and len(f)>=2:
            res.append((f[0][2],bytes(int(d,16) for d,_,_ in f[1:])))
    return res

if __name__=='__main__':
    for t,m in messages(sys.argv[1]):
        print(f'{t:8.3f} n={len(m):3d} {m.hex(" ")[:200]}')
