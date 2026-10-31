#!/usr/bin/env python3
"""Exercise USB snapshots on the connected HudBuddy; restore real HUD afterwards.

Run with project Python from the repository root. Does not reset or wake the board.
"""
import argparse
import json
import sys
import time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from service.bridge import HudClient, DEFAULT_CLI, discover_port, open_serial, message, encode_line, MAX_LINE_BYTES

p=argparse.ArgumentParser()
p.add_argument('--idle', type=int, default=0, help='seconds to observe sleep while sending heartbeats')
args=p.parse_args()
client=HudClient(DEFAULT_CLI)
client.check_compatibility()
real=client.fetch()
port=discover_port()
assert port, 'HudBuddy USB not found'
s=open_serial(port)
rx=bytearray()
events=[]
def poll(seconds):
    deadline=time.monotonic()+seconds
    found=[]
    while time.monotonic()<deadline:
        rx.extend(s.read(8192))
        while b'\n' in rx:
            raw,_,remaining=rx.partition(b'\n');rx[:]=remaining
            try: obj=json.loads(raw)
            except ValueError:
                print('boot:',raw.decode(errors='replace'),flush=True);continue
            print(obj,flush=True);found.append(obj);events.append(obj)
        time.sleep(.01)
    return found
def send(payload):
    raw=encode_line(payload)
    start=time.monotonic();assert s.write(raw)==len(raw)
    print('WRITE',len(raw),'bytes',round(time.monotonic()-start,3),'seconds',flush=True)
def snapshot(seq, tasks):
    send(message('snapshot',seq=seq,tasks=tasks))
    reply=poll(2)
    assert any(x.get('type')=='ack' and x.get('seq')==seq for x in reply),f'No ACK for {seq}'
try:
    send(message('heartbeat'));poll(1)
    snapshot(901,real)
    invalid=[dict(real[0],status='invalid')] if real else [{'id':'bad'}]
    send(message('snapshot',seq=902,tasks=invalid))
    assert not any(x.get('type')=='ack' and x.get('seq')==902 for x in poll(.5)), 'Invalid task was accepted'
    # Max-size valid line, then oversized garbage and a valid line to recover framing.
    large=[{'id':str(i),'title':f'USB 测试任务 {i+1}','status':'doing' if i%2==0 else 'waiting','summary':'','updated_at':0} for i in range(64)]
    payload=message('snapshot',seq=903,tasks=large)
    missing=MAX_LINE_BYTES-len(encode_line(payload))
    large[0]['summary']='x'*missing
    assert len(encode_line(payload))==MAX_LINE_BYTES
    send(payload)
    assert any(x.get('type')=='ack' and x.get('seq')==903 for x in poll(3)), 'Max-size snapshot not accepted'
    s.write(b'x'*(MAX_LINE_BYTES+1)+b'\n')
    poll(.3)
    snapshot(904,real)
    print('PASS: real data, invalid task rejection, 64 tasks / 100KiB boundary, oversized recovery',flush=True)
    end=time.monotonic()+args.idle
    saver_synced=False
    while time.monotonic()<end:
        send(message('heartbeat'));poll(min(3,max(0,end-time.monotonic())))
        if not saver_synced and any(x.get('event')=='screensaver' for x in events):
            snapshot(906,real);saver_synced=True
    if args.idle>=125:
        assert any(x.get('event')=='screensaver' for x in events), 'No screensaver telemetry'
        assert any(x.get('type')=='telemetry' and x.get('awake') is False for x in events), 'No sleep telemetry (check whether user touched screen)'
        snapshot(905,real)
        assert any(x.get('event')=='snapshot' and x.get('awake') is False for x in events), 'Snapshot did not stay asleep'
        print('PASS: screensaver then sleep despite USB heartbeats; saver/asleep snapshots acknowledged without waking',flush=True)
finally:
    # Restore live data even if a boundary test failed.
    try: send(message('snapshot',seq=999,tasks=real));poll(.5)
    finally: s.close()
