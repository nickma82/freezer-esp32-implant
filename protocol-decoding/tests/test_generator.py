import pytest
import protocol as P
from protocol import decode,Msg,Rec

CAPTURED={   # bytes after the break, copied from the captures
 'superfrost_on' :(P.SUPERFROST_ON ,'02 FF 02 24 01 04 0C 00 20 04 5D 69'),
 'superfrost_off':(P.SUPERFROST_OFF,'02 FF 02 24 01 04 0C 00 20 01 F8 39'),
 'party_on'      :(P.PARTY_ON      ,'02 FF 02 24 01 04 08 02 20 02 0A AD'),
 'party_off'     :(P.PARTY_OFF     ,'02 FF 02 24 01 04 08 02 20 00 48 8D'),
}

@pytest.mark.parametrize('name',CAPTURED)
def test_command_matches_capture(name):
    fn,expected=CAPTURED[name]
    assert fn().hex(' ').upper()==expected

@pytest.mark.parametrize('degc,expected',[
    (-17,'02 FF 02 24 01 05 0C 06 40 00 EF D8 99'),
    (-16,'02 FF 02 24 01 05 0C 06 40 00 F0 06 7A'),
])
def test_setpoint_matches_capture(degc,expected):
    assert P.SETPOINT(degc).hex(' ').upper()==expected

def test_read_request_matches_capture():
    params=[(0x0C,0x00,0x20),(0x0C,0x06,0x40),(0x0C,0x07,0x40),(0x0C,0x10,0x20),
            (0x0C,0x2B,0x40),(0x0C,0x14,0x40),(0x0C,0x08,0x20)]
    exp='01 FF 01 03 01 15 0C 00 20 0C 06 40 0C 07 40 0C 10 20 0C 2B 40 0C 14 40 0C 08 20 9C 58'
    assert P.read_request(0x01,params).hex(' ').upper()==exp

def test_generated_frames_decode_back():
    m=decode(P.SETPOINT(-17))
    assert m.kind=='values' and m.crc_ok
    assert m.recs==[Rec(0x0C,0x06,0x40,b'\x00\xEF')]

def test_setpoint_is_signed_int8():
    assert decode(P.SETPOINT(-18)).recs[0].value==b'\x00\xEE'
    assert decode(P.SETPOINT(5)).recs[0].value==b'\x00\x05'

def test_write_param_widths():
    assert decode(P.write_param(0x0C,0x00,0x20,4)).recs[0].value==b'\x04'
    assert decode(P.write_param(0x0C,0x06,0x40,0x00EF)).recs[0].value==b'\x00\xEF'
    assert decode(P.write_param(0x0C,0x3A,0x80,0xC0080000)).recs[0].value==bytes.fromhex('C0080000')

def test_value_too_large_for_flag_is_rejected():
    with pytest.raises(OverflowError):
        P.write_param(0x0C,0x00,0x20,0x1FF)

def test_unknown_flag_is_rejected():
    with pytest.raises(KeyError):
        P.write_param(0x0C,0x00,0x33,1)

def test_line_timing_constants():
    assert (P.BREAK_US,P.POST_BREAK_US,P.BAUD)==(316,90,57600)
