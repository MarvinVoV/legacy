#!/usr/bin/env python3
"""Real HTTP -> MQTT -> S3 -> STM32 lighting regression; optional suspended motion.

Run from pyserver using PYTHONPATH=. .venv/bin/python. No ground/optical accuracy
claim: pixels are computed frame values reported by STM32, not a light sensor.
"""
import argparse
import json
import threading
import time
from pathlib import Path

import httpx
import paho.mqtt.client as mqtt
from app.config import get_settings


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suspended",action="store_true",help="Allow 80mm/s wheels-lifted cases")
    parser.add_argument("--output",type=Path,required=True)
    args=parser.parse_args()
    settings=get_settings()
    lock=threading.Condition();ready=threading.Event();events=[];cases=[]
    observer=mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,client_id=f"lighting-hil-{time.time_ns()}")
    if settings.mqtt_username:observer.username_pw_set(settings.mqtt_username,settings.mqtt_password)
    observer.on_connect=lambda c,*_:c.subscribe("data/DEV_1/#")
    observer.on_subscribe=lambda *_:ready.set()
    def receive(c,u,msg):
        try:data=json.loads(msg.payload)
        except (ValueError,UnicodeError):return
        kind=msg.topic.rsplit("/",1)[-1]
        if kind=="sensor" and data.get("type") not in ("lighting","battery"):return
        with lock:
            events.append({"at":time.monotonic(),"kind":kind,"data":data});lock.notify_all()
    observer.on_message=receive;observer.connect(settings.mqtt_host,settings.mqtt_port);observer.loop_start()
    report={"passed":False,"motion":args.suspended}
    with httpx.Client(base_url="http://127.0.0.1:8000/api/v1",trust_env=False,timeout=10,
                      headers={"X-API-Key":settings.api_key} if settings.api_key else {}) as api:
        def post(path,body=None):
            r=api.post(path,json=body);r.raise_for_status();return r.json()
        def stop():
            t=time.monotonic();r=post("/agent/devices/DEV_1/stop")
            assert r.get("confirmed") and r.get("stopped"),r
            return {"ms":round((time.monotonic()-t)*1000,1),**r}
        def wait(predicate,since,timeout=5,kind="sensor"):
            end=time.monotonic()+timeout
            with lock:
                while True:
                    for e in reversed(events):
                        if e["at"]>=since and e["kind"]==kind and predicate(e["data"]):return e["data"]
                    left=end-time.monotonic();assert left>0,"missing "+kind+" evidence"
                    lock.wait(left)
        def light(predicate,since,timeout=5):
            return wait(lambda d:d.get("type")=="lighting" and predicate(d["data"]),since,timeout)["data"]
        def hint(body):
            r=post("/devices/DEV_1/lighting",body)
            assert r.get("success") and r["data"]=={"accepted":True,"confirmed":False},r
            return r
        try:
            assert ready.wait(5)
            for _ in range(40):
                state=api.get("/devices/DEV_1/status").json()["data"]
                if state["online"] and state["stm32_connected"] and state.get("lighting"):break
                time.sleep(1)
            else:raise AssertionError("lighting/device readiness timeout")
            report["initial_stop"]=stop()
            epoch=(state["lighting"]["epoch"]+1)&0xFFFFFFFF
            for mood in ("happy","curious","comforting","sad","calm"):
                since=time.monotonic()
                hint({"emotion":mood,"duration_ms":2000,"epoch":epoch})
                data=light(lambda d:d["emotion"]==mood and d["flags"]&4,since)
                assert all(0<=v<=12 for v in data["pixels"]),data
                cases.append({"case":"emotion_"+mood,"sample":data})
                print("emotion "+mood+" PASS",flush=True)
            since=time.monotonic()
            hint({"emotion":"happy","duration_ms":1500,"epoch":epoch})
            light(lambda d:d["emotion"]=="happy" and d["flags"]&4,since)
            data=light(lambda d:d["emotion"]=="calm" and not d["flags"]&4,since,5)
            cases.append({"case":"ttl_fallback","sample":data})
            baseline=data["rejected"]
            since=time.monotonic()
            hint({"emotion":"sad","duration_ms":2000,"epoch":(epoch-1)&0xFFFFFFFF})
            data=light(lambda d:d["rejected"]!=baseline,since)
            assert data["emotion"]=="calm",data
            cases.append({"case":"old_epoch_rejected","sample":data})

            if args.suspended:
                for seq,name,speed,curve,mode in (
                    (190,"forward",80,0,1),(191,"reverse",-80,0,2),
                    (192,"left",80,-1,3),(193,"right",80,1,4)):
                    since=time.monotonic()
                    hint({"emotion":"happy","duration_ms":4000,"epoch":epoch})
                    for _ in range(10):
                        r=post("/devices/DEV_1/commands",{"seq":seq,"cmd":"motion","motion":{
                            "mode":3,"params":{"linear_vel":speed,"curvature_1pm":curve}}})
                        assert r["success"],r
                        time.sleep(.15)
                    data=light(lambda d:d["mode"]==mode and d["emotion"]=="happy",since)
                    result=stop()
                    brake=light(lambda d:d["mode"]==5,since,3)
                    assert brake["pixels"][0]>0 and not brake["pixels"][1] and not brake["pixels"][2]
                    cases.append({"case":"local_"+name+"_overrides_mood","sample":data,"stop":result})
                    print(name+" override PASS",flush=True)
                    light(lambda d:d["mode"]==0,since,3)

                # Flood decoration independently of STOP; a one-slot S3 mailbox coalesces.
                for _ in range(3):
                    post("/devices/DEV_1/commands",{"seq":194,"cmd":"motion","motion":{
                        "mode":3,"params":{"linear_vel":80,"curvature_1pm":0}}})
                    time.sleep(.15)
                def flood():
                    for _ in range(150):
                        observer.publish("cmd/DEV_1/exec",json.dumps({"cmd":"lighting","emotion":"curious",
                            "intensity":100,"duration_ms":2000,"epoch":epoch}),qos=0)
                        time.sleep(.004)
                worker=threading.Thread(target=flood,daemon=True);worker.start()
                result=stop();worker.join(3);assert not worker.is_alive()
                cases.append({"case":"stop_during_250hz_visual_flood","stop":result})
                print("STOP during visual flood PASS",flush=True)

            since=time.monotonic()
            hint({"clear":True,"epoch":(epoch+1)&0xFFFFFFFF})
            data=light(lambda d:not d["flags"]&12 and d["emotion"]=="calm" and d["mode"]==0,since)
            cases.append({"case":"clear","sample":data})
            time.sleep(12) # Complete timing/statistics window after motion/flood.
            with lock:messages=[e["data"].get("message","") for e in events if e["kind"]=="log"]
            timing=[s for s in messages if s.startswith("RTOS ")]
            assert timing and all("miss=0 " in s for s in timing),timing
            perf=[s for s in messages if s.startswith("PERF cpu_")]
            assert perf and all("rx_drop=0 " in s and "log_drop=0" in s for s in perf),perf
            led=[s for s in messages if s.startswith("LED frames=")]
            assert led and all("errors=0" in s for s in led),led
            status=api.get("/devices/DEV_1/status").json()["data"]
            assert status["battery"]["available"] and status["imu_age_s"]<1,status
            report.update(passed=True,timing=timing,runtime=perf,led=led,final_status=status)
        except Exception as exc:report["failure"]=f"{type(exc).__name__}: {exc}"
        finally:
            try:report["final_stop"]=stop()
            except Exception as exc:report.update(passed=False,final_stop_error=str(exc))
            observer.disconnect();observer.loop_stop()
            report.update(cases=cases,events=events)
            args.output.parent.mkdir(parents=True,exist_ok=True)
            args.output.write_text(json.dumps(report,ensure_ascii=False,indent=2)+"\n")
    print(json.dumps({k:v for k,v in report.items() if k not in ("events","cases","final_status","timing")},ensure_ascii=False),flush=True)
    return 0 if report["passed"] else 1


if __name__=="__main__":raise SystemExit(main())
