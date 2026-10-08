#!/usr/bin/env python3
"""
Record service — the thing that can say whether a shipment is certifiable.

Subscribes to the node, stores every reading, and continuously answers the
only question that matters: is the sequence unbroken?

  python3 record_service.py --broker localhost --device node1

It writes an SQLite file so the record survives this process being restarted,
and prints a certificate on demand:

  python3 record_service.py --certificate S0007
"""
import argparse, json, sqlite3, sys, time
from datetime import datetime, timezone

import paho.mqtt.client as mqtt

DB = "coldchain.db"

SCHEMA = """
CREATE TABLE IF NOT EXISTS reading (
  sid TEXT NOT NULL, seq INTEGER NOT NULL,
  ts INTEGER NOT NULL, t REAL NOT NULL, h INTEGER,
  flags TEXT NOT NULL DEFAULT '',
  received INTEGER NOT NULL,
  PRIMARY KEY (sid, seq)
);
CREATE TABLE IF NOT EXISTS event (
  sid TEXT, seq INTEGER, ts INTEGER, kind TEXT, value REAL, received INTEGER
);
CREATE TABLE IF NOT EXISTS anomaly (
  sid TEXT, seq INTEGER, kind TEXT, detail TEXT, received INTEGER
);
CREATE TABLE IF NOT EXISTS shipment (
  sid TEXT PRIMARY KEY, started INTEGER, tmin REAL, tmax REAL, period INTEGER
);
"""


def connect_db():
    db = sqlite3.connect(DB)
    db.executescript(SCHEMA)
    return db


def note(db, sid, seq, kind, detail):
    db.execute("INSERT INTO anomaly VALUES (?,?,?,?,?)",
               (sid, seq, kind, detail, int(time.time())))
    db.commit()
    print(f"  !! {kind}: {detail}", file=sys.stderr)


def on_reading(db, payload):
    """Store one reading. Duplicates are expected after a reconnection — QoS 1
    is at-least-once — so the only thing that matters is whether a repeated
    seq carries the same content. A contradiction destroys the record."""
    r = json.loads(payload)
    sid, seq = r["sid"], int(r["seq"])
    row = (sid, seq, int(r["ts"]), float(r["t"]), int(r.get("h", -1)),
           r.get("f", ""), int(time.time()))

    prev = db.execute("SELECT ts,t,h FROM reading WHERE sid=? AND seq=?",
                      (sid, seq)).fetchone()
    if prev is None:
        db.execute("INSERT INTO reading VALUES (?,?,?,?,?,?,?)", row)
        db.commit()
        flag = f" [{r.get('f')}]" if r.get("f") else ""
        print(f"{sid} #{seq:<5} {r['t']:6.2f} C  rh={r.get('h')}%{flag}")
    else:
        same = (prev[0] == row[2] and abs(prev[1] - row[3]) < 1e-9 and prev[2] == row[4])
        if same:
            print(f"{sid} #{seq:<5} duplicate, identical — ignored")
        else:
            note(db, sid, seq, "contradiction",
                 f"seq {seq} first said {prev}, now {row[2:5]}")


def on_event(db, payload):
    e = json.loads(payload)
    db.execute("INSERT INTO event VALUES (?,?,?,?,?,?)",
               (e.get("sid"), e.get("seq"), e.get("ts"), e.get("e"),
                e.get("v"), int(time.time())))
    db.commit()
    print(f"  event {e.get('e')}  sid={e.get('sid')} seq={e.get('seq')} v={e.get('v')}")
    if e.get("e") == "buffer_overrun":
        note(db, e.get("sid"), e.get("seq"), "overrun",
             "the node overwrote a record before it could be delivered")


def on_shipment(db, payload):
    s = json.loads(payload)
    db.execute("INSERT OR REPLACE INTO shipment VALUES (?,?,?,?,?)",
               (s["sid"], s.get("ts"), s.get("tmin"), s.get("tmax"), s.get("period")))
    db.commit()
    print(f"== shipment {s['sid']}  band {s.get('tmin')}..{s.get('tmax')} C  "
          f"every {s.get('period')} s")


def gaps(db, sid):
    """The whole point of the project: which sequence numbers are missing."""
    seqs = [r[0] for r in db.execute(
        "SELECT seq FROM reading WHERE sid=? ORDER BY seq", (sid,))]
    if not seqs:
        return None, []
    missing, expected = [], set(range(0, seqs[-1] + 1))
    missing = sorted(expected - set(seqs))
    return seqs[-1], missing


def certificate(db, sid):
    last, missing = gaps(db, sid)
    if last is None:
        print(f"no readings for {sid}")
        return 1

    n = db.execute("SELECT COUNT(*) FROM reading WHERE sid=?", (sid,)).fetchone()[0]
    anomalies = db.execute("SELECT kind,detail FROM anomaly WHERE sid=?", (sid,)).fetchall()
    exc = db.execute("SELECT COUNT(*) FROM reading WHERE sid=? AND flags LIKE '%X%'",
                     (sid,)).fetchone()[0]
    rep = db.execute("SELECT COUNT(*) FROM reading WHERE sid=? AND flags LIKE '%B%'",
                     (sid,)).fetchone()[0]
    tmin, tmax = db.execute("SELECT MIN(t),MAX(t) FROM reading WHERE sid=?",
                            (sid,)).fetchone()
    t0, t1 = db.execute("SELECT MIN(ts),MAX(ts) FROM reading WHERE sid=?",
                        (sid,)).fetchone()

    # a clock that goes backwards inside a shipment invalidates the ordering
    backwards = db.execute(
        "SELECT COUNT(*) FROM reading a JOIN reading b "
        "ON a.sid=b.sid AND b.seq=a.seq+1 WHERE a.sid=? AND b.ts < a.ts",
        (sid,)).fetchone()[0]

    def when(x):
        return datetime.fromtimestamp(x, timezone.utc).strftime("%Y-%m-%d %H:%M:%SZ")

    ok = (not missing) and (not anomalies) and backwards == 0
    print()
    print(f"  SHIPMENT {sid}")
    print(f"  {'-' * 56}")
    print(f"  readings            {n}  (seq 0..{last})")
    print(f"  period              {when(t0)}  ->  {when(t1)}")
    print(f"  temperature         {tmin:.2f} .. {tmax:.2f} C")
    print(f"  excursions          {exc} reading(s) outside the band")
    print(f"  replayed offline    {rep} reading(s)")
    print(f"  missing             {len(missing)}" + (f"  {missing[:12]}" if missing else ""))
    print(f"  clock reversals     {backwards}")
    for k, d in anomalies:
        print(f"  anomaly             {k}: {d}")
    print(f"  {'-' * 56}")
    print(f"  RECORD {'COMPLETE — certifiable' if ok else 'INCOMPLETE — not certifiable'}")
    print()
    return 0 if ok else 2


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--broker", default="localhost")
    ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("--device", default="node1")
    ap.add_argument("--certificate", metavar="SID",
                    help="print the certificate for a shipment and exit")
    a = ap.parse_args()

    db = connect_db()
    if a.certificate:
        sys.exit(certificate(db, a.certificate))

    root = f"coldchain/{a.device}"

    def on_connect(c, u, f, rc, props=None):
        print(f"connected to {a.broker}, watching {root}/#")
        # QoS 1 on the subscription too: a reading dropped between the broker
        # and this service would be just as much of a hole.
        c.subscribe([(f"{root}/reading", 1), (f"{root}/event", 1),
                     (f"{root}/shipment", 1), (f"{root}/status", 1)])

    def on_message(c, u, m):
        try:
            topic = m.topic.rsplit("/", 1)[-1]
            if topic == "reading":    on_reading(db, m.payload)
            elif topic == "event":    on_event(db, m.payload)
            elif topic == "shipment": on_shipment(db, m.payload)
            elif topic == "status":
                s = m.payload.decode()
                print(f"** node is {s.upper()}")
                if s == "offline":
                    note(db, None, None, "node_offline",
                         "the broker published the Last Will: the node disappeared")
        except Exception as e:                      # never let one bad payload stop the service
            note(db, None, None, "bad_payload", f"{m.topic}: {e}")

    try:
        c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    except AttributeError:                          # paho 1.x
        c = mqtt.Client()
    c.on_connect, c.on_message = on_connect, on_message
    c.connect(a.broker, a.port, keepalive=30)
    c.loop_forever()


if __name__ == "__main__":
    main()
