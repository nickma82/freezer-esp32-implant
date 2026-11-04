import pytest
from protocol import decode,encode,interpret,Msg,Rec

def b(s): return bytes.fromhex(s.replace(' ',''))

def test_bare_command():
    m=decode(b('7F'))
    assert m.kind=='bare' and m.cmd==0x7F and m.crc_ok is None

def test_idle_ack():
    m=decode(b('01 FF 01 30 00 02 00 00 5F 82'))
    assert m.kind=='ack' and m.cmd==0x01 and m.crc_ok and m.recs==[]

def test_read_request_list_has_no_values():
    m=decode(b('01 FF 01 03 01 09 0C 00 20 0C 06 40 0C 14 40 21 80'))
    assert m.kind=='list' and m.crc_ok
    assert [(r.grp,r.id,r.flag,r.value) for r in m.recs]==[(0x0C,0x00,0x20,b''),(0x0C,0x06,0x40,b''),(0x0C,0x14,0x40,b'')]

def test_values_reply_with_four_byte_record():
    m=decode(b('82 82 7F 13 01 70 08 14 20 00 08 20 40 E0 16 08 31 80 F6 25 08 06 08 32 20 03 08 13 20 1B 08 14 20 00 08 16 40 10 0E 08 04 80 FB 8A 6A 05 08 51 20 00 08 2D 80 00 00 00 00 08 2E 20 00 08 14 20 00 08 17 40 E8 03 08 2F 80 00 00 00 00 08 30 20 00 08 18 20 02 08 2A 80 6A 45 39 08 08 33 40 1B 00 08 59 20 00 08 2B 80 00 00 00 00 04 01 40 00 00 04 02 40 29 00 0A 46'))
    assert m.kind=='values' and m.crc_ok and len(m.recs)==22
    r={(x.grp,x.id):x for x in m.recs}
    assert r[(0x08,0x31)].value==b('F6 25 08 06') and r[(0x04,0x02)].value==b('29 00')

def test_flag_00_means_parameter_not_available():
    m=decode(b('82 82 7F 13 01 16 0C 3B 40 72 01 0C 01 20 03 60 0C 00 60 0D 00 0C 27 20 00 0C F9 00 F2 1E'))
    assert m.crc_ok
    gone=[(r.grp,r.id) for r in m.recs if r.flag==0x00]
    assert gone==[(0x60,0x0C),(0x60,0x0D),(0x0C,0xF9)] and all(r.value==b'' for r in m.recs if r.flag==0)

def test_wrong_crc_is_reported_not_raised():
    f=bytearray(b('01 FF 01 30 00 02 00 00 5F 82')); f[-1]^=0xFF
    assert decode(bytes(f)).crc_ok is False

def test_length_field_mismatch_raises():
    f=b('01 FF 01 03 01 0A 0C 00 20 0C 06 40 0C 14 40 21 80')   # claims 10 bytes, has 9
    with pytest.raises(ValueError):
        decode(f)

def test_unknown_flag_raises():
    body=b('FF 02 24 01 04 0C 00 77 04')
    from crc import crc16_xmodem
    with pytest.raises(ValueError):
        decode(bytes([0x02])+body+crc16_xmodem(body).to_bytes(2,'little'))

@pytest.mark.parametrize('frame',[
    '01 FF 01 30 00 02 00 00 5F 82',
    '02 FF 02 24 01 05 0C 06 40 00 F0 06 7A',
    '01 FF 01 03 01 09 0C 00 20 0C 06 40 0C 14 40 21 80',
])
def test_roundtrip(frame):
    assert encode(decode(b(frame)))==b(frame)

# --- semantics ------------------------------------------------------------------------
def test_interpret_status_values():
    m=decode(b('81 81 7F 13 01 20 0C 00 20 04 0C 06 40 00 EE 0C 07 40 9A FF 0C 10 20 00 0C 2B 40 60 00 0C 14 40 70 EE 0C 08 20 05 05 AC'))
    got=dict(interpret(m))
    assert got=={'mode':'superfrost','setpoint':-18,'current_temp':-18}

def test_interpret_door_alarm_param():
    m=decode(b('81 81 7F 13 01 4E 0C 4C 20 00 0C 47 20 00 0C 09 20 01 0C 11 20 00 0C 0D 20 00 0C 0F 20 00 0C 46 20 00 0C 4B 20 00 0C 12 40 00 C9 0C 2E 40 00 BD 0C 0C 40 00 C9 0C 2F 40 00 BD 0C 0E 40 00 00 0C 30 40 00 00 0C 45 20 00 0C 58 20 00 0C 49 20 00 0C 0B 20 00 3C 3B'))
    assert dict(interpret(m))=={'door_alarm':True}

def test_interpret_event_flag_block():
    quiet=decode(b('FF 80 7F 24 01 20 00 00 80 00 00 00 00 00 01 80 00 00 00 00 00 02 80 00 00 00 00 00 03 80 00 00 00 00 00 04 20 20 18 7F'))
    assert quiet.crc_ok and interpret(quiet)==[]
    # door-alarm flag pair 00000100 / 08000000 is built by editing the quiet block and recomputing the CRC
    body=b('80 7F 24 01 20 00 00 80 00 00 01 00 00 01 80 08 00 00 00 00 02 80 00 00 00 00 00 03 80 00 00 00 00 00 04 20 20')
    from crc import crc16_xmodem
    ev=decode(bytes([0xFF])+body+crc16_xmodem(body).to_bytes(2,'little'))
    assert interpret(ev)==[('event','door_alarm_changed')]


# --- bootup: strings, new float-less flags ------------------------------------------------
from protocol import identity, wifi, text

def _vals(*recs):
    return decode(encode(Msg(0x02,0xFF,0x02,(0x24,0x01),'values',list(recs)))).recs

def _chunks(grp,first,s):
    s=s.encode().ljust((len(s)+3)//4*4,b'\0')
    return [Rec(grp,first+i//4,0x80,s[i:i+4]) for i in range(0,len(s),4)]

def test_flags_82_and_84_are_four_bytes():
    r=_vals(Rec(0x4C,1,0x82,b'\x54\x55\x82\x24'),Rec(0x4C,2,0x84,b'\x8c\xb3\xa0\x24'))
    assert [x.value for x in r]==[b'\x54\x55\x82\x24',b'\x8c\xb3\xa0\x24']

def test_unknown_tag_passes_through_unchanged():
    raw=bytes.fromhex('8b8b7f2101007' '3da')            # captured, tag 21 01 not understood
    m=decode(raw); assert m.kind=='unknown' and encode(m)==raw

def test_text_joins_records_and_drops_nul_padding():
    assert text(_chunks(0x3C,1,'FNc    5076'),0x3C,1,3)=='FNc    5076'

def test_identity_from_the_captured_reply():
    raw=bytes.fromhex('82827f13016d3c00201d3c0180464e63203c0280202020353c0380303736003c048000'
                      '0000003c05803230412f3c0680303031003c0780313233343c0880353637383c098039'
                      '0000003c0d80393933353c0e80363732303c0f80310000003c1080393837363c118035'
                      '3433323c128031000000ddf9')
    m=decode(raw); assert m.crc_ok and encode(m)==raw
    i=identity(m.recs)
    assert i['model']=='FNc    5076' and i['index']=='20A/001' and i['number_a']=='123456789'
    assert i['part_no_plain']=='993567201' and i['number_b']=='987654321'

def test_wifi_record_from_the_box():
    recs=_chunks(0x48,4,'192.168.1.100@Example_Wifi')+[Rec(0x48,0x14,0x80,bytes.fromhex('6401a8c0'))]
    w=wifi(recs); assert w=={'text':'192.168.1.100@Example_Wifi','ip':'192.168.1.100'}
    assert wifi([Rec(0x48,0x00,0x20,b'\x03')])=={}
