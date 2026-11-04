"""Replay the real captures through the parser/generator. Needs csv/ (decoded exports) and raw-captures/digital.csv."""
import csv,collections
import pytest
import protocol as P
from conftest import CSV,RAW
from frames import messages
from protocol import decode,encode
from timeline import timeline

FILES=sorted(p for p in CSV.glob('*_hex.csv') if not p.name.startswith(('ch1','ch2')))
pytestmark=pytest.mark.skipif(not FILES,reason='no exported captures in csv/')

def msgs(name): return messages(CSV/f'{name}_hex.csv')
def events(name): return [(round(t,2),k,v) for t,k,v in timeline(CSV/f'{name}_hex.csv')]
def seq(name,key): return [v for t,k,v in timeline(CSV/f'{name}_hex.csv') if k==key]

# --- whole-corpus properties ---------------------------------------------------------------
def test_every_message_decodes_and_reencodes_identically():
    kinds=collections.Counter();bad_crc=0
    for p in FILES:
        for t,b in messages(p):
            m=decode(b)                       # must not raise
            kinds[m.kind]+=1
            if m.crc_ok is False: bad_crc+=1
            elif m.kind!='bare': assert encode(m)==b,(p.name,t)
    assert kinds['ack']>400 and kinds['list']>80 and kinds['values']>200 and kinds['bare']>1000
    assert bad_crc<=1      # the one frame cut off by the end of a capture

def test_all_checked_frames_pass_crc():
    ok=total=0
    for p in FILES:
        for t,b in messages(p):
            m=decode(b)
            if m.crc_ok is not None: total+=1; ok+=m.crc_ok
    assert total>700 and total-ok<=1

def test_breaks_are_18_bit_times_long():
    """The framing-error 0x00 is a UART break: ~18.2 bit times low, ~5 bit times high before the next byte."""
    f=RAW/'digital.csv'
    if not f.exists(): pytest.skip('raw-captures/digital.csv missing')
    rows=list(csv.reader(open(f)))[1:]
    t=[float(r[0]) for r in rows];v=[int(r[1]) for r in rows]
    bit=1/P.BAUD
    lows=[(t[i],t[i+1]-t[i]) for i in range(len(t)-1) if v[i]==0]
    starts=[d for k,(a,d) in enumerate(lows) if k==0 or a-(lows[k-1][0]+lows[k-1][1])>0.002]
    assert len(starts)>=100
    assert all(17.5<d/bit<19 for d in starts)
    assert abs(P.BREAK_US-sum(starts)/len(starts)*1e6)<5

# --- the generator reproduces what the box really sent --------------------------------------
@pytest.mark.parametrize('capture,frame',[
    ('superfrost_enable',P.SUPERFROST_ON()),  ('superfrost_disable',P.SUPERFROST_OFF()),
    ('bottletimer_enable',P.SUPERFROST_ON()), ('bottletimer_disable',P.SUPERFROST_OFF()),
    ('partymode_enable',P.PARTY_ON()),        ('partymode_disable',P.PARTY_OFF()),
    ('setpoint_-18_to_-17',P.SETPOINT(-17)),  ('setpoint_-17_to_-16',P.SETPOINT(-16)),
])
def test_capture_contains_the_generated_command(capture,frame):
    assert any(b==frame for t,b in msgs(capture))

def test_other_directions_are_not_present():
    assert not any(b==P.SUPERFROST_ON() for t,b in msgs('superfrost_disable'))
    assert not any(b==P.PARTY_ON() for t,b in msgs('partymode_disable'))

# --- interpreted timelines -------------------------------------------------------------------
def test_superfrost_enable_timeline():
    ev=events('superfrost_enable')
    assert ('mode','superfrost') in [(k,v) for _,k,v in ev]
    t_mode=next(t for t,k,v in ev if (k,v)==('mode','superfrost'))
    t_evt=next(t for t,k,v in ev if (k,v)==('event','value_changed'))
    assert 0<t_evt-t_mode<0.2          # the freezer raises the changed-flag right after the write

def test_superfrost_disable_returns_to_normal():
    assert seq('superfrost_disable','mode')[-1]=='normal'

def test_party_mode_raises_its_own_event_flag():
    for name,state in (('partymode_enable',True),('partymode_disable',False)):
        assert seq(name,'party')[-1]==state
        assert 'party_changed' in seq(name,'event')

def test_party_enable_switches_superfrost_on_by_itself():
    assert seq('partymode_enable','mode')[-1]=='superfrost'
    assert seq('partymode_disable','mode')[-1]=='normal'

def test_setpoint_steps():
    assert seq('setpoint_-18_to_-17','setpoint')==[-18,-17]
    assert seq('setpoint_-17_to_-16','setpoint')==[-17,-16]
    assert set(seq('setpoint_-18_to_-17','current_temp'))=={-18}     # current temperature did not move

def test_door_alarm_on():
    ev=events('door_alarm_on')
    assert seq('door_alarm_on','door_alarm')==[False,True]
    t_evt=next(t for t,k,v in ev if (k,v)==('event','door_alarm_changed'))
    t_on=next(t for t,k,v in ev if (k,v)==('door_alarm',True))
    assert 0<t_on-t_evt<1               # flag first, then the box reads the alarm parameter
    assert t_evt>1.0                    # 'somewhere in the middle' of the 4.3 s capture

def test_door_alarm_off_with_door_closed():
    assert 'door_alarm_changed' in seq('door_alarm_off','event')
    assert seq('door_alarm_off','door_alarm')==[False]
    assert seq('door_alarm_off','door_open')[-1] is False

def test_long_capture_goes_through_a_whole_alarm_cycle():
    ev=events('door_open_alarm')
    assert seq('door_open_alarm','door_alarm')==[False,True,False]
    assert seq('door_open_alarm','event').count('door_alarm_changed')==2
    assert seq('door_open_alarm','door_open')==[True,False]

def test_short_door_open_has_no_alarm():
    assert seq('door_short_open','door_open')==[False,True]
    assert True not in seq('door_short_open','door_alarm')
    assert 'door_alarm_changed' not in seq('door_short_open','event')

def test_short_door_close_has_no_alarm():
    assert seq('door_short_closed','door_open')==[True,False]
    assert 'door_alarm_changed' not in seq('door_short_closed','event')


def test_bootup_identity_and_wifi_strings():
    """The bootup export carries the freezer identification (group 3C) and the box's network record (group 48)."""
    from protocol import identity, wifi
    ids=[];nets=[];versions=[]
    for t,b in messages(CSV/'freezer_bootup_hex.csv'):
        m=decode(b)
        if m.kind=='values':
            if identity(m.recs): ids.append(identity(m.recs))
            if wifi(m.recs): nets.append(wifi(m.recs))
        elif m.tag==(0x16,0x01): versions.append(b[6:-2].decode().rstrip('\0'))
    merged={}                                          # the freezer pages the group over several replies
    for i in ids: merged.update({k:v for k,v in i.items() if v})
    assert merged=={'model':'FNc    5076','index':'20A/001','number_a':'123456789','part_no':'9935672-01',
                    'part_no_plain':'993567201','number_b':'987654321'}
    assert nets and nets[0]['text']=='192.168.1.100@Example_Wifi' and nets[0]['ip']=='192.168.1.100'
    assert versions[0]=='TOD 5-600|SW 1-86-7|BUS 1-4-0' and versions[-1]=='TOD 5-600|SW 6-2-0|BUS 1-4-0'
