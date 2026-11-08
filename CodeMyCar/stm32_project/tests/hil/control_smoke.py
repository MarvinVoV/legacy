"""Low-speed suspended-car smoke test. Run explicitly with --suspended.

Requires a running pyserver/Mosquitto and paho-mqtt (pyserver/.venv).
No physical yaw/steering-angle accuracy claim is made while wheels are lifted.
"""
import argparse
import json
import os
import threading
import time
import urllib.error
import urllib.request

import paho.mqtt.client as mqtt


class Capture:
    def __init__(self, device):
        self.events = []
        self.lock = threading.Condition()
        self.ready = threading.Event()
        self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                                  client_id=f"cmcar-hil-{os.getpid()}")
        def connect(c, userdata, flags, reason, properties):
            c.subscribe(f"data/{device}/#")
            c.subscribe(f"cmd/{device}/exec")
            self.ready.set()
        def message(c, userdata, msg):
            try:
                data = json.loads(msg.payload)
            except (ValueError, UnicodeDecodeError):
                return
            with self.lock:
                self.events.append((time.monotonic(), msg.topic.rsplit('/',1)[-1], data))
                self.lock.notify_all()
        self.client.on_connect = connect
        self.client.on_message = message
        self.client.connect('127.0.0.1',1883)
        self.client.loop_start()
        assert self.ready.wait(3), 'broker not connected'

    def wait(self, kind, predicate, since, timeout=3):
        end = time.monotonic()+timeout
        with self.lock:
            while True:
                for stamp, topic, data in reversed(self.events):
                    if stamp >= since and topic == kind and predicate(data):
                        return data
                left = end-time.monotonic()
                if left <= 0:
                    raise AssertionError(f'missing {kind} confirmation')
                self.lock.wait(left)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--suspended', action='store_true', required=True,
                        help='Explicitly confirm wheels are suspended and motor power is on')
    parser.add_argument('--device',default='DEV_1')
    args = parser.parse_args()
    capture = Capture(args.device)
    base = f'http://127.0.0.1:8000/api/v1'
    results = []

    def request(path, body=None, post=False):
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(base+path,data=data,
            headers={'Content-Type':'application/json'},method='POST' if post else 'GET')
        with urllib.request.urlopen(req,timeout=12) as r:
            return json.load(r)

    def stop():
        before=time.monotonic()
        result=request(f'/agent/devices/{args.device}/stop',post=True)
        assert result.get('confirmed') and result.get('stopped'),result
        latency=round((time.monotonic()-before)*1000,1)
        if latency>800:
            with capture.lock:
                trace=[{'after_ms':round((t-before)*1000,1),'kind':kind,
                        'seq':data.get('seq'),'mode':data.get('mode'),
                        'left':data.get('left_mmps'),'right':data.get('right_mmps')}
                       for t,kind,data in capture.events if t>=before and
                       (kind in ('exec','ack') or (kind=='motion' and data.get('mode')==0))]
            print('SLOW_STOP_TRACE',json.dumps(trace),flush=True)
        return latency

    def report(name, **data):
        result={'case':name,'pass':True,**data}
        results.append(result)
        print(json.dumps(result),flush=True)

    def command(seq,mode,params):
        start=time.monotonic()
        response=request(f'/devices/{args.device}/commands',
            {'seq':seq,'cmd':'motion','motion':{'mode':mode,'params':params}},post=True)
        assert response.get('success'),response
        ack=capture.wait('ack',lambda x:x.get('seq')==seq,start)
        assert ack.get('status')==0,ack

    try:
        report('initial_stop',ms=stop())
        capture.wait('sensor',lambda x:x.get('type')=='ultrasonic' and
                     x.get('data',{}).get('distance',0)>40 and
                     x.get('data',{}).get('distance_rear',0)>40,time.monotonic()-1,5)
        cases=[('direct_forward',1,{'left_rpm':30,'right_rpm':30,'curvature_1pm':0},1),
               ('direct_reverse',1,{'left_rpm':-30,'right_rpm':-30,'curvature_1pm':0},-1),
               ('drive_forward',3,{'linear_vel':80,'curvature_1pm':0},1),
               ('drive_reverse',3,{'linear_vel':-80,'curvature_1pm':0},-1),
               ('curve_left',3,{'linear_vel':80,'curvature_1pm':-1},1),
               ('curve_right',3,{'linear_vel':80,'curvature_1pm':1},1)]
        for index,(name,mode,params,direction) in enumerate(cases):
            seq=200+index
            start=time.monotonic()
            for _ in range(5):
                command(seq,mode,params)
                time.sleep(.15)
            sample=capture.wait('motion',lambda x:x.get('seq')==seq and
                direction*x.get('left_mmps',0)>10 and direction*x.get('right_mmps',0)>10,
                start)
            assert not sample.get('fault'),sample
            latency=stop()
            report(name,left_mmps=sample['left_mmps'],right_mmps=sample['right_mmps'],stop_ms=latency)
            time.sleep(.2)

        start=time.monotonic()
        command(210,3,{'linear_vel':80,'curvature_1pm':0})
        sample=capture.wait('motion',lambda x:x.get('seq')==210 and
            x.get('mode')==0 and x.get('lease_expired') and
            abs(x.get('left_mmps',100))<=1 and abs(x.get('right_mmps',100))<=1,start,3)
        report('lease_expiry',after_ms=round((time.monotonic()-start)*1000,1))
        stop()

        result=request(f'/agent/devices/{args.device}/move',
            {'distance_cm':5,'turn_deg':0,'speed_cm_s':5},post=True)
        assert result.get('ok'),result
        report('goal_5cm_encoder_only',measured=result.get('measured'))
        stop()

        aborted=[]
        def running_goal():
            try:
                aborted.append(request(f'/agent/devices/{args.device}/move',
                    {'distance_cm':20,'turn_deg':0,'speed_cm_s':5},post=True))
            except urllib.error.HTTPError as exc:
                aborted.append(json.load(exc))
        start=time.monotonic()
        worker=threading.Thread(target=running_goal,daemon=True)
        worker.start()
        capture.wait('motion',lambda x:x.get('mode')==4 and x.get('state')=='running',start)
        time.sleep(.25)
        latency=stop()
        worker.join(2)
        assert not worker.is_alive() and aborted,aborted
        assert aborted[0].get('detail',{}).get('code')=='aborted_by_stop',aborted
        report('interrupt_running_goal',stop_ms=latency,result=aborted[0])

        latencies=[stop() for _ in range(10)]
        report('stop_x10',min_ms=min(latencies),max_ms=max(latencies))
        print(json.dumps({'total_passed':len(results),'results':results}),flush=True)
    finally:
        try:
            print('FINAL_STOP',stop(),flush=True)
        finally:
            capture.client.disconnect()
            capture.client.loop_stop()


if __name__=='__main__':
    main()
