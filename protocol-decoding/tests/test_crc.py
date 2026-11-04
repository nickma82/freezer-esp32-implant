import pytest
from crc import crc16_xmodem,check

def test_standard_check_value():
    # CRC-16/XMODEM catalogue check value for "123456789"
    assert crc16_xmodem(b'123456789')==0x31C3

@pytest.mark.parametrize('frame',[
    '02 FF 02 24 01 04 0C 00 20 04 5D 69',      # SuperFrost on
    '01 FF 01 30 00 02 00 00 5F 82',            # idle ack of the 01 poll
    '0B FF 0B 24 01 04 58 07 20 11 9A 48',      # status byte, door closed
])
def test_captured_frames_pass(frame):
    assert check(bytes.fromhex(frame))

def test_crc_covers_everything_after_the_command_byte():
    f=bytearray.fromhex('02 FF 02 24 01 04 0C 00 20 04 5D 69')
    f[0]=0x7E                       # the command byte is not part of the checksum
    assert check(bytes(f))
    f[9]^=0x01                      # any other bit is
    assert not check(bytes(f))

def test_crc_is_stored_low_byte_first():
    body=bytes.fromhex('FF 02 24 01 04 0C 00 20 04')
    c=crc16_xmodem(body)
    assert c==0x695D and bytes.fromhex('5D 69')==c.to_bytes(2,'little')
