"""Suspended-car GOAL source probe. Body is expected to stay on its support.

A small curved goal runs at 5 cm/s; encoder yaw can change while body yaw
remains unchanged. IMU/encoder disagreement must not produce a false success.
"""
import argparse
import json
import time
import urllib.request
from control_smoke import Capture

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--suspended',action='store_true',required=True)
    parser.parse_args()
    capture=Capture('DEV_1')
    def post(path,body=None):
        request=urllib.request.Request('http://127.0.0.1:8000/api/v1'+path,
            data=json.dumps(body).encode() if body is not None else None,
            headers={'Content-Type':'application/json'},method='POST')
        with urllib.request.urlopen(request,timeout=12) as res:return json.load(res)
    def stop():
        res=post('/agent/devices/DEV_1/stop')
        assert res.get('confirmed') and res.get('stopped'),res
        return res
    try:
        start=time.monotonic();stop()
        baseline=capture.wait('motion',lambda x:x.get('mode')==0,start)
        start=time.monotonic()
        response=post('/devices/DEV_1/commands',{'seq':241,'cmd':'motion',
            'motion':{'mode':4,'params':{'turn_ddeg':100,'distance_mm':200,
                                      'speed_mm_s':50}}})
        assert response.get('success'),response
        ack=capture.wait('ack',lambda x:x.get('seq')==241,start)
        assert ack.get('status')==0,ack
        terminal=capture.wait('motion',lambda x:x.get('seq')==241 and
                             x.get('state') in ('done','error'),start,9)
        assert terminal.get('goal_heading_source')=='imu',terminal
        # Fail the fixture assumption explicitly if the supported body rotated.
        assert abs(terminal['goal_turn_mdeg'])<2000,terminal
        assert terminal.get('state')=='error' and terminal.get('error')==5,terminal
        encoder_turn=-(terminal['theta_mdeg']-baseline['theta_mdeg'])/1000
        assert abs(encoder_turn-terminal['goal_turn_mdeg']/1000)>2,terminal
        print(json.dumps({'pass':True,'encoder_turn_deg':encoder_turn,
            'imu_turn_deg':terminal['goal_turn_mdeg']/1000,'terminal':terminal}),flush=True)
    finally:
        try:print('FINAL_STOP',json.dumps(stop()),flush=True)
        finally:
            capture.client.disconnect();capture.client.loop_stop()
if __name__=='__main__':main()
