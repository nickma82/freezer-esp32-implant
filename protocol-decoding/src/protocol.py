"""SmartDevice <-> freezer MCU protocol: frame parser and encoder.
A message on the wire is: BREAK, <cmd>, <body...>, <crc lo>, <crc hi>.
decode()/encode() work on the bytes after the break (cmd first).
Roles (05-bus-roles.md): BREAK and <cmd> (the slot byte) are sent by the freezer; the body is sent by the slot owner.
In the body, `marker` is the destination (0x80|node, FF = freezer) and `second` the source node.
The write_param() frames start with slot byte 02: the box itself only sends the bytes after it."""
from dataclasses import dataclass, field
from crc import crc16_xmodem

WIDTH={0x00:0,0x20:1,0x21:1,0x40:2,0x41:2,0x80:4,0x82:4,0x84:4}
TAGS={(0x30,0x00):'ack',(0x30,0x01):'ack',(0x03,0x01):'list',(0x24,0x01):'values',(0x13,0x01):'values'}

@dataclass
class Rec:
    grp:int; id:int; flag:int; value:bytes=b''      # value empty in a descriptor list
@dataclass
class Msg:
    cmd:int; marker:int|None=None; second:int|None=None; tag:tuple=(); kind:str='bare'
    recs:list=field(default_factory=list); raw:bytes=b''; crc_ok:bool|None=None

def parse_records(b,with_values):
    out=[];i=0
    while i<len(b):
        if i+3>len(b): raise ValueError('truncated record')
        g,idn,fl=b[i],b[i+1],b[i+2]
        if fl not in WIDTH: raise ValueError(f'unknown flag {fl:02X} at {i}')
        n=WIDTH[fl] if with_values else 0
        if i+3+n>len(b): raise ValueError('truncated value')
        out.append(Rec(g,idn,fl,bytes(b[i+3:i+3+n])));i+=3+n
    return out

def decode(frame:bytes)->Msg:
    m=Msg(cmd=frame[0],raw=bytes(frame))
    if len(frame)==1: return m                       # bare 0C / 7E / 7F
    m.crc_ok=len(frame)>=4 and crc16_xmodem(frame[1:-2])==int.from_bytes(frame[-2:],'little')
    body=frame[1:-2]
    m.marker=body[0]; m.second=body[1]; m.tag=(body[2],body[3])
    kind=TAGS.get(m.tag)
    if kind is None: m.kind='unknown'; return m
    if kind=='ack': m.kind='ack'; return m
    ln=body[4]; payload=body[5:]
    if ln!=len(payload): raise ValueError(f'length {ln} != {len(payload)}')
    m.kind='list' if kind=='list' else 'values'
    m.recs=parse_records(payload,with_values=(kind=='values'))
    return m

def encode(m:Msg)->bytes:
    if m.kind=='bare': return bytes([m.cmd])
    if m.kind=='unknown': return m.raw               # tag not understood: pass through unchanged
    body=bytes([m.marker,m.second,*m.tag])
    if m.kind!='ack':
        payload=b''.join(bytes([r.grp,r.id,r.flag])+r.value for r in m.recs)
        body+=bytes([len(payload)])+payload
    else:
        body+=m.raw[1+4:-2]
    return bytes([m.cmd])+body+crc16_xmodem(body).to_bytes(2,'little')

# --- generator helpers -------------------------------------------------------------
def write_param(grp,pid,flag,value:int)->bytes:
    v=value.to_bytes(WIDTH[flag],'big')
    return encode(Msg(0x02,0xFF,0x02,(0x24,0x01),'values',[Rec(grp,pid,flag,v)]))
def read_request(cmd,params)->bytes:
    return encode(Msg(cmd,0xFF,cmd,(0x03,0x01),'list',[Rec(g,i,f) for g,i,f in params]))
SUPERFROST_ON =lambda:write_param(0x0C,0x00,0x20,0x04)
SUPERFROST_OFF=lambda:write_param(0x0C,0x00,0x20,0x01)
PARTY_ON      =lambda:write_param(0x08,0x02,0x20,0x02)
PARTY_OFF     =lambda:write_param(0x08,0x02,0x20,0x00)
SETPOINT      =lambda c:write_param(0x0C,0x06,0x40,c&0xFF)
BREAK_US,POST_BREAK_US,BAUD=316,90,57600

# --- semantics (only what the captures prove) ----------------------------------------
KNOWN={(0x0C,0x00):'mode',(0x08,0x02):'party',(0x0C,0x06):'setpoint',(0x0C,0x14):'current_temp',
       (0x0C,0x31):'door_open',(0x0C,0x09):'door_alarm'}
EVENT_FLAGS={(0x01000000,0x08000000):'value_changed',(0x00800000,0x04000000):'party_changed',
             (0x00000100,0x08000000):'door_alarm_changed'}
def interpret(m:Msg):
    """Return a list of (name, value) for known params, or [('event', name)] for the 1 s event-flag block."""
    if m.kind!='values': return []
    ids={(r.grp,r.id):r.value for r in m.recs}
    if m.marker==0x80 and m.second==0x7F and (0x00,0x00) in ids:       # FF 80 7F 24 01 20 ... block
        key=(int.from_bytes(ids[(0,0)],'big'),int.from_bytes(ids[(0,1)],'big'))
        return [('event',EVENT_FLAGS.get(key,f'unknown_{key[0]:08X}_{key[1]:08X}'))] if any(key) else []
    out=[]
    for k,v in ids.items():
        n=KNOWN.get(k)
        if n=='mode': out.append((n,{1:'normal',4:'superfrost'}.get(v[0],v[0])))
        elif n=='party': out.append((n,bool(v[0]) ))
        elif n in('setpoint','current_temp'): out.append((n,int.from_bytes(v[1:],'big',signed=True)))
        elif n in('door_open','door_alarm'): out.append((n,bool(v[0])))
    return out

# --- text blocks (bootup) ------------------------------------------------------------
# Strings travel as consecutive flag-80 records of 4 ASCII bytes each (NUL padded), in id order.
IDENTITY={'model':(0x01,0x03),'index':(0x05,0x06),'number_a':(0x07,0x09),'part_no':(0x0A,0x0C),
          'part_no_plain':(0x0D,0x0F),'number_b':(0x10,0x12)}      # group 3C, read from the freezer
WIFI_GROUP,WIFI_TEXT=0x48,(0x04,0x0B)                               # group 48, written by the box
def text(recs,grp,lo,hi):
    """ASCII string held in records grp/lo..hi (flag 80, 4 bytes each); NUL padding dropped."""
    chunks=[r.value for r in sorted(recs,key=lambda r:r.id) if r.grp==grp and lo<=r.id<=hi and r.flag==0x80]
    return b''.join(chunks).replace(b'\0',b'').decode('ascii')
def identity(recs):
    """Freezer identification strings (group 3C) found in a values message, {} if the group is absent."""
    if not any(r.grp==0x3C for r in recs): return {}
    return {k:text(recs,0x3C,lo,hi) for k,(lo,hi) in IDENTITY.items()}
def wifi(recs):
    """Box -> freezer network record (group 48): {'text': 'ip@ssid', 'ip': dotted quad} or {}."""
    t=text(recs,WIFI_GROUP,*WIFI_TEXT)
    if not t: return {}
    ip=next((r.value for r in recs if (r.grp,r.id)==(WIFI_GROUP,0x14)),None)
    return {'text':t,'ip':'.'.join(str(b) for b in reversed(ip)) if ip else None}
