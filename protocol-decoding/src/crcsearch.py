import itertools
frames_=[ # bytes after the leading 00; last 2 = crc
"02 FF 02 24 01 04 0C 00 20 04 5D 69","02 FF 02 24 01 04 0C 00 20 01 F8 39",
"02 FF 02 24 01 04 08 02 20 02 0A AD","02 FF 02 24 01 04 08 02 20 00 48 8D",
"01 FF 01 30 00 02 00 00 5F 82","0B FF 0B 30 00 02 00 00 5D 04",
"0B FF 0B 24 01 04 58 07 20 11 9A 48","0B FF 0B 24 01 04 58 07 20 01 AB 5A",
"01 FF 01 03 01 15 0C 00 20 0C 06 40 0C 07 40 0C 10 20 0C 2B 40 0C 14 40 0C 08 20 9C 58"]
fr=[bytes.fromhex(x.replace(' ','')) for x in frames_]
def reflect(v,w):
    r=0
    for i in range(w):
        if v>>i&1: r|=1<<(w-1-i)
    return r
def crc16(data,poly,init,refin,refout,xo):
    c=init
    for b in data:
        if refin: b=reflect(b,8)
        c^=b<<8
        for _ in range(8):
            c=((c<<1)^poly)&0xFFFF if c&0x8000 else (c<<1)&0xFFFF
    if refout:c=reflect(c,16)
    return c^xo
hits=[]
for poly in range(1,0x10000,2):
    for init in (0,0xFFFF,0x1D0F,0xB2AA):
        for ri in (0,1):
            for ro in (0,1):
                for xo in (0,0xFFFF):
                    for start in (0,1,2):
                        for endian in ('be','le'):
                            ok=True
                            for f in fr:
                                d=f[start:-2];got=crc16(d,poly,init,ri,ro,xo)
                                exp=int.from_bytes(f[-2:],endian[0:1]=='b' and 'big' or 'little')
                                if got!=exp: ok=False;break
                            if ok: hits.append((hex(poly),hex(init),ri,ro,hex(xo),start,endian))
print(hits[:10],len(hits))
