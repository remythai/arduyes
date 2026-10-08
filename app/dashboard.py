#!/usr/bin/env python3
"""
Dashboard — the live view of the record, as a terminal strip.

  python3 dashboard.py --broker localhost --device node1

Draws the same picture as the project brief: one block per reading, coloured
by how it arrived. A hole in the sequence is drawn as a gap, because a hole
is the one thing the whole system exists to prevent.

  block  delivered live, in band
  block  replayed from the node's buffer after an outage
  block  temperature excursion
  gap    MISSING — never acceptable
"""
import argparse, json, shutil, sys, time
import paho.mqtt.client as mqtt

RESET = "\033[0m"
LIVE = "\033[38;5;31m"      # steel blue
BUF = "\033[38;5;178m"      # amber
EXC = "\033[38;5;160m"      # red
GAP = "\033[38;5;240m"      # grey
DIM = "\033[2m"
BOLD = "\033[1m"

state = {
    "sid": None, "band": (None, None), "period": None,
    "status": "unknown", "readings": {}, "events": [], "last_rx": 0,
}


def draw():
    cols = shutil.get_terminal_size((100, 30)).columns
    r = state["readings"]
    sys.stdout.write("\033[H\033[J")                     # home, clear

    lo, hi = state["band"]
    band = f"{lo} .. {hi} C" if lo is not None else "unknown"
    dot = {"online": "\033[38;5;34m●", "offline": "\033[38;5;160m●"}.get(state["status"], "\033[38;5;240m●")
    print(f"{BOLD}COLD CHAIN{RESET}   shipment {state['sid'] or '—'}   band {band}"
          f"   {dot} {state['status']}{RESET}")
    print(DIM + "─" * min(cols, 100) + RESET)

    if not r:
        print("\n  waiting for the first reading…")
        return

    last = max(r)
    missing = [s for s in range(last + 1) if s not in r]
    strip = []
    for s in range(last + 1):
        if s not in r:
            strip.append(GAP + "·")
        else:
            f = r[s]["f"]
            c = EXC if "X" in f else (BUF if "B" in f else LIVE)
            strip.append(c + "█")
    # keep the most recent readings visible when the strip gets long
    width = min(cols - 4, 96)
    shown = strip[-width:]
    print("\n  " + "".join(shown) + RESET)
    print(f"  {DIM}seq {max(0, last - len(shown) + 1)} … {last}{RESET}\n")

    live = sum(1 for v in r.values() if "B" not in v["f"] and "X" not in v["f"])
    rep = sum(1 for v in r.values() if "B" in v["f"])
    exc = sum(1 for v in r.values() if "X" in v["f"])
    temps = [v["t"] for v in r.values()]
    print(f"  readings {len(r)} of {last + 1}      "
          f"{LIVE}live {live}{RESET}   {BUF}replayed {rep}{RESET}   {EXC}excursions {exc}{RESET}")
    print(f"  temperature {min(temps):.2f} .. {max(temps):.2f} C      "
          f"last {r[last]['t']:.2f} C")

    if missing:
        print(f"\n  {EXC}{BOLD}RECORD INCOMPLETE{RESET} — {len(missing)} missing: "
              f"{missing[:10]}{' …' if len(missing) > 10 else ''}")
    else:
        print(f"\n  {LIVE}{BOLD}sequence unbroken{RESET} — every reading from 0 to {last} is present")

    if state["events"]:
        print(f"\n  {DIM}recent events{RESET}")
        for e in state["events"][-4:]:
            print(f"    {DIM}seq {str(e.get('seq')):>5}{RESET}  {e.get('e')}"
                  + (f"  {e.get('v')}" if e.get("v") is not None else ""))

    if state["last_rx"]:
        age = int(time.time() - state["last_rx"])
        if age > 5:
            print(f"\n  {DIM}nothing received for {age} s{RESET}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--broker", default="localhost")
    ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("--device", default="node1")
    a = ap.parse_args()
    root = f"coldchain/{a.device}"

    def on_connect(c, u, f, rc, props=None):
        c.subscribe([(f"{root}/reading", 1), (f"{root}/event", 1),
                     (f"{root}/shipment", 1), (f"{root}/status", 1)])

    def on_message(c, u, m):
        leaf = m.topic.rsplit("/", 1)[-1]
        state["last_rx"] = time.time()
        try:
            if leaf == "status":
                state["status"] = m.payload.decode()
            elif leaf == "shipment":
                s = json.loads(m.payload)
                if s["sid"] != state["sid"]:
                    state["readings"].clear(); state["events"].clear()
                state["sid"] = s["sid"]
                state["band"] = (s.get("tmin"), s.get("tmax"))
                state["period"] = s.get("period")
            elif leaf == "reading":
                d = json.loads(m.payload)
                state["readings"][int(d["seq"])] = {"t": float(d["t"]), "f": d.get("f", "")}
            elif leaf == "event":
                state["events"].append(json.loads(m.payload))
        except Exception:
            pass                                   # a malformed payload must not kill the view
        draw()

    try:
        c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    except AttributeError:
        c = mqtt.Client()
    c.on_connect, c.on_message = on_connect, on_message
    c.connect(a.broker, a.port, keepalive=30)
    draw()
    try:
        c.loop_forever()
    except KeyboardInterrupt:
        print(RESET)


if __name__ == "__main__":
    main()
