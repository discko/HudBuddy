#!/usr/bin/env python3
"""Read-only source queries; temporary device fixtures, then restore real data.

Stop Bridge before running with the project Python. No reset-card action exists.
"""
import copy
import json
import sys
import time
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from service.bridge import HudClient,DEFAULT_CLI,open_serial,discover_port,message,encode_line
from service.magpie import MagpieClient

client=HudClient(DEFAULT_CLI);client.check_compatibility()
tasks=client.fetch();quota=MagpieClient().fetch()
port=discover_port();assert port,'Buddy not found'
s=open_serial(port);rx=bytearray()
def poll(seconds):
    found=[];end=time.monotonic()+seconds
    while time.monotonic()<end:
        rx.extend(s.read(8192))
        while b'\n' in rx:
            raw,_,rest=rx.partition(b'\n');rx[:]=rest
            try:d=json.loads(raw)
            except ValueError:continue
            print(d,flush=True);found.append(d)
        time.sleep(.02)
    return found
def send(seq,**fields):
    s.write(encode_line(message('snapshot',seq=seq,**fields)))
    result=poll(1)
    assert any(x.get('type')=='ack' and x.get('seq')==seq for x in result),f'No ACK: {seq}'
    return result
try:
    s.write(encode_line(message('heartbeat')));poll(.2)
    send(810,tasks=tasks,hud_ok=True,quota=quota)
    now=time.time_ns()//1000000
    fixture=copy.deepcopy(quota)
    fixture.update(available=True,stale=True,last_success_ms=now-25*3600000,
                   monitor_started_ms=now-48*3600000,
                   cards={'count':3,'expires_at_ms':[now+11*3600000,now+60*3600000,None]},
                   five_hour={'remaining':87,'resets_at_ms':now+90*60000})
    result=send(811,quota=fixture)
    assert all(x.get('tasks')==len(tasks) for x in result if x.get('event')=='snapshot'),'Quota-only update erased HUD'
    malformed=copy.deepcopy(fixture);malformed['cards']['count']=1
    s.write(encode_line(message('snapshot',seq=812,quota=malformed)))
    assert not any(x.get('type')=='ack' and x.get('seq')==812 for x in poll(.5))
    result=send(813,quota={'available':False,'stale':True,'last_success_ms':None,'monitor_started_ms':now-25*3600000})
    assert all(x.get('tasks')==len(tasks) for x in result if x.get('event')=='snapshot')
    send(814,tasks=tasks,hud_ok=True,quota=quota)
    s.write(encode_line(message('heartbeat',magpie_last_success_ms=now,magpie_monitor_started_ms=now)))
    poll(.5)
    print('PASS: real quota, multi-card/5h/stale fixture, quota-only preserves HUD, invalid quota rejected, real data restored',flush=True)
finally:
    try:s.write(encode_line(message('snapshot',seq=819,tasks=tasks,hud_ok=True,quota=quota)));poll(.5)
    finally:s.close()
