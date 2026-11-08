"""Suspended low-speed STOP-under-burst regression; no movement after confirmation."""
import argparse
import json
import time
import urllib.request
from control_smoke import Capture

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--suspended', action='store_true', required=True)
    parser.add_argument('--cycles', type=int, default=10)
    args = parser.parse_args()
    capture = Capture('DEV_1')
    base = 'http://127.0.0.1:8000/api/v1'
    def request(path, body=None):
        req = urllib.request.Request(base+path, method='POST',
            data=json.dumps(body).encode() if body is not None else None,
            headers={'Content-Type':'application/json'})
        with urllib.request.urlopen(req, timeout=12) as res:
            return json.load(res)
    def stop():
        before = time.monotonic()
        res = request('/agent/devices/DEV_1/stop')
        assert res.get('confirmed') and res.get('stopped'), res
        return round((time.monotonic()-before)*1000, 1)
    try:
        print('INITIAL_STOP', stop(), flush=True)
        # Positive angular demand is unsafe on the production Ackermann chassis.
        start = time.monotonic()
        request('/devices/DEV_1/commands', {'seq':240,'cmd':'motion',
            'motion':{'mode':2,'params':{'linear_vel':0,'angular_vel':10}}})
        ack = capture.wait('ack',lambda x:x.get('seq')==240,start)
        assert ack.get('status')==3, ack
        print('DIFFERENTIAL_REJECTED', json.dumps(ack), flush=True)
        latencies = []
        for cycle in range(args.cycles):
            rpm = 30 if cycle % 2 == 0 else -30
            command = {'seq':90,'cmd':'motion','motion':{'mode':1,
                'params':{'left_rpm':rpm,'right_rpm':rpm,'curvature_1pm':0}}}
            start = time.monotonic()
            request('/devices/DEV_1/commands', command)
            capture.wait('motion',lambda x:x.get('mode')==1 and
                abs(x.get('left_mmps',0))>10,start)
            # Flood from one MQTT connection; final QoS1 PUBACK ensures broker
            # accepted preceding TCP data before the HTTP STOP is requested.
            for index in range(40):
                command['seq'] = 100+index
                publish = capture.client.publish('cmd/DEV_1/exec',
                    json.dumps(command),qos=1 if index==39 else 0)
            publish.wait_for_publish(timeout=3)
            assert publish.is_published()
            ms = stop()
            confirmed = time.monotonic()
            time.sleep(1.3)
            with capture.lock:
                samples = [x for stamp,kind,x in capture.events
                           if stamp>=confirmed and kind=='motion']
            assert len(samples)>=3, samples
            assert all(x.get('mode')==0 and abs(x.get('left_mmps',999))<=1 and
                       abs(x.get('right_mmps',999))<=1 and not x.get('fault')
                       for x in samples), samples
            latencies.append(ms)
            print(json.dumps({'cycle':cycle+1,'pass':True,'burst':40,
                'stop_ms':ms,'quiet_samples':len(samples)}),flush=True)
        print(json.dumps({'passed':len(latencies),'min_stop_ms':min(latencies),
                          'max_stop_ms':max(latencies)}),flush=True)
    finally:
        try:
            print('FINAL_STOP',stop(),flush=True)
        finally:
            capture.client.disconnect()
            capture.client.loop_stop()
if __name__=='__main__':
    main()
