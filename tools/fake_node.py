#!/usr/bin/env python3
"""
Stub node — lets the broker, service and dashboard be built and tested before
the Arduino exists, and lets the measurement campaign run without hardware.

  python3 fake_node.py --outage 20:40 --excursion 55:60 --drop 12,13

  --outage A:B     stop publishing between seq A and B, then replay them
                   (this is what the real node does after losing WiFi)
  --excursion A:B  mark those readings as outside the band
  --drop 12,13     never publish those seq at all, to prove the service
                   actually detects a hole
"""
import argparse, json, random, time
import paho.mqtt.client as mqtt

ap = argparse.ArgumentParser()
ap.add_argument("--broker", default="localhost")
ap.add_argument("--device", default="node1")
ap.add_argument("--sid", default="S9001")
ap.add_argument("--count", type=int, default=80)
ap.add_argument("--period", type=float, default=0.25)
ap.add_argument("--outage", default="")
ap.add_argument("--excursion", default="")
ap.add_argument("--drop", default="")
a = ap.parse_args()

span = lambda s: (lambda p: (int(p[0]), int(p[1])))(s.split(":")) if s else (-1, -2)
out_a, out_b = span(a.outage)
exc_a, exc_b = span(a.excursion)
dropped = {int(x) for x in a.drop.split(",") if x.strip()}

root = f"coldchain/{a.device}"
try:
    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
except AttributeError:
    c = mqtt.Client()
c.will_set(f"{root}/status", "offline", qos=1, retain=True)
c.connect(a.broker, 1883, 30)
c.loop_start()
c.publish(f"{root}/status", "online", qos=1, retain=True)
c.publish(f"{root}/shipment", json.dumps(
    {"sid": a.sid, "ts": int(time.time()), "tmin": 2.0, "tmax": 8.0, "period": 60}),
    qos=1, retain=True)

held = []
for seq in range(a.count):
    exc = exc_a <= seq <= exc_b
    t = round(random.uniform(9.0, 11.0) if exc else random.uniform(3.0, 6.5), 2)
    rec = {"sid": a.sid, "seq": seq, "ts": int(time.time()), "t": t, "h": 48,
           "f": "X" if exc else ""}
    if seq in dropped:
        print(f"#{seq} dropped on purpose — the service should report a hole")
    elif out_a <= seq <= out_b:
        held.append(rec)
        print(f"#{seq} buffered (offline)")
    else:
        if held:                                   # link is back: replay in order
            for r in held:
                r["f"] = (r["f"] + "B")
                c.publish(f"{root}/reading", json.dumps(r), qos=1)
                print(f"#{r['seq']} replayed")
                time.sleep(0.05)
            held.clear()
        c.publish(f"{root}/reading", json.dumps(rec), qos=1)
        print(f"#{seq} {t} C" + ("  EXCURSION" if exc else ""))
    time.sleep(a.period)

time.sleep(1)
c.publish(f"{root}/status", "online", qos=1, retain=True)
c.loop_stop()
