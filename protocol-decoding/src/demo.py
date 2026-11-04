"""Show the frames the generator produces (bytes after the break) and how the line is driven."""
import protocol as P

def show(name,frame): print(f'{name:16s}',frame.hex(' ').upper())

if __name__=='__main__':
    print(f'line: the freezer sends BREAK {P.BREAK_US} us low, {P.POST_BREAK_US} us high, slot byte 02;')
    print(f'      the box answers ~426 us later with the bytes after the 02, at {P.BAUD} 8N1\n')
    show('SuperFrost on',P.SUPERFROST_ON()); show('SuperFrost off',P.SUPERFROST_OFF())
    show('Party on',P.PARTY_ON()); show('Party off',P.PARTY_OFF())
    for c in (-18,-17,-16): show(f'Set-point {c}',P.SETPOINT(c))
    show('Read mode+temp',P.read_request(0x01,[(0x0C,0x00,0x20),(0x0C,0x06,0x40),(0x0C,0x14,0x40)]))
