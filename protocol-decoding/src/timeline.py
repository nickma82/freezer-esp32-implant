"""Print a readable timeline of the known parameters/events in one or more exported captures."""
import sys
from frames import messages
from protocol import decode,interpret

def timeline(path):
    last={};out=[]
    for t,b in messages(path):
        if len(b)<5: continue
        for k,v in interpret(decode(b)):
            if k=='event' or last.get(k)!=v: out.append((t,k,v))
            last[k]=v
    return out

if __name__=='__main__':
    for p in sys.argv[1:]:
        print('==',p)
        for t,k,v in timeline(p): print(f'{t:6.2f}s {k}={v}')
