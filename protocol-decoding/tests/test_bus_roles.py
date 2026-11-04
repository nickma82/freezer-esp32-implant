"""Bus-role findings of 05-bus-roles.md, checked against the real captures."""
import collections, statistics as st
import pytest
from conftest import CSV,RAW
from slots import bursts
import clocks

FILES=sorted(CSV.glob('*_hex.csv'))
pytestmark=pytest.mark.skipif(not FILES,reason='no exported captures in csv/')

def bodies():
    for p in FILES:
        for t,b,ts in bursts(p):
            if len(b)>=6: yield p.name,t,b,ts

def test_address_model_dst_src():
    """Slot < 0x80: the slot owner sends (src == slot). Slot >= 0x80: the master 7F sends to node slot & 0x7F."""
    odd=[]
    for name,t,b,ts in bodies():
        slot,dst,src=b[0],b[1],b[2]
        if slot&0x80:
            ok=src==0x7F and (dst==slot or (slot==0xFF and dst==0x80))
        else:
            ok=src==slot and dst&0x80
        if not ok: odd.append((name,round(t,3),bytes(b[:3]).hex(' ')))
    assert odd==[('freezer_bootup_hex.csv',4.008,'01 ff 0b')]   # the single 01/0B mix-up at start-up

def test_bare_slots_are_never_answered():
    seen=collections.Counter();answered=collections.Counter()
    for p in FILES:
        for t,b,ts in bursts(p):
            if b and b[0] in (0x0C,0x7E,0x7F): seen[b[0]]+=1; answered[b[0]]+=len(b)>1
    assert min(seen.values())>700 and sum(answered.values())==0

def test_three_responder_latency_profiles():
    lat=collections.defaultdict(list)
    for name,t,b,ts in bodies(): lat[b[2]].append((ts[1]-ts[0])*1e6)
    assert 240<st.median(lat[0x7F])<260                 # master answers in its own slots
    assert 420<st.median(lat[0x02])<432                 # box
    assert 600<st.median(lat[0x01])<680 and 600<st.median(lat[0x0B])<680

@pytest.mark.parametrize('name',['digital.csv','freezer-bootup-fullduration.csv'])
def test_slot_bytes_use_the_masters_clock(name):
    """Clock fingerprint: slot bytes have node 7F's bit period, not the box's."""
    f=RAW/name
    if not f.exists(): pytest.skip(f'raw-captures/{name} missing')
    per=collections.defaultdict(list)
    for m in clocks.split(clocks.edges(f)):
        bs=m['bytes']
        for i,(val,falls,lows) in enumerate(bs):
            role='slot' if i==0 else (bs[2][0] if len(bs)>3 else None)
            per[role]+=[dt/n*1e6 for dt,n in falls if n>=4]
    slot,master,box=(st.median(per[k]) for k in ('slot',0x7F,0x02))
    assert abs(slot-master)<0.002
    assert slot-box>0.007

def test_node_01_0B_low_power_clock_follows_the_door():
    """Node 01/0B (one MCU, not 7F) runs a jittery clock from ~5.6 s after door close (33.27 s) until door open (54.78 s)."""
    f=RAW/'freezer-bootup-fullduration.csv'
    if not f.exists(): pytest.skip('raw-captures/freezer-bootup-fullduration.csv missing')
    S=clocks.states(clocks.split(clocks.edges(f)))
    assert [s for s,*_ in S[0x7F]]==['clean'] and [s for s,*_ in S[0x02]]==['clean']
    for node in (0x01,0x0B):
        assert [s for s,*_ in S[node]]==['clean','jitter','clean']
        _,start,end,_=S[node][1]
        assert 38.7<start<39.1 and 54.7<end<54.95

def test_node_01_0B_sleeps_while_the_door_is_closed():
    """Latency view of the same states, over all decoded captures: low-power only with the door closed for a while."""
    from slots import ui_states
    st_={p.name:[s for s,*_ in ui_states(p)] for p in FILES}
    for name in ('door_alarm_on_hex.csv','door_alarm_off_hex.csv','door_open_alarm_hex.csv','door_short_closed_hex.csv'):
        assert st_[name]==['awake']                                  # door open, or closed < 2.5 s before the end
    assert st_['door_short_open_hex.csv']==['low-power','awake']     # wakes after the door opens
    assert st_['freezer_bootup_hex.csv']==['awake','low-power','awake']
    assert st_['door_open_close_nop30_hex.csv']==['low-power','awake','low-power']   # stays asleep for 26 s
    from slots import door_pushes
    for name in ('door_open_close_nop30_hex.csv','freezer_bootup_hex.csv'):  # timeout: 5-6 s after the door closes
        closed=[t for t,o in door_pushes(CSV/name) if not o]
        sleep=[a for s,a,*_ in ui_states(CSV/name) if s=='low-power' and a>closed[0]][0]
        assert any(4.9<sleep-t<5.8 for t in closed)
    for name in ('superfrost_enable_hex.csv','partymode_enable_hex.csv','setpoint_-18_to_-17_hex.csv'):
        assert st_[name]==['low-power']                              # app writes from the box do not wake it
