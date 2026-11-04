def crc16_xmodem(data):
    c=0
    for b in data:
        c^=b<<8
        for _ in range(8):
            c=((c<<1)^0x1021)&0xFFFF if c&0x8000 else (c<<1)&0xFFFF
    return c
def check(frame):
    """frame: bytes of one burst without the leading 00. Returns True if the trailing LE CRC matches."""
    return crc16_xmodem(frame[1:-2])==int.from_bytes(frame[-2:],'little')
