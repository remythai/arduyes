# Cold chain — provably complete temperature record

Four programs. The node records, the broker distributes, the service judges
whether the record is certifiable, the dashboard shows it.

```
[Uno R4 WiFi] --WiFi/TCP/MQTT--> [mosquitto] --> record_service.py  (SQLite + certificate)
                                            \--> dashboard.py       (live strip)
```

There is no gateway. The R4 WiFi is a network client in its own right, so the
only reliability code we write is the part TCP cannot provide — sequence
numbers, non-volatile buffering and ordered replay.

| Path | Owner | What it is |
|---|---|---|
| `docs/topics.md` | all | **The frozen contract.** Change it here before touching any program. |
| `node/node.ino`, `node/config.h` | Node & logging | Firmware. Every tunable is in `config.h`. |
| `broker/mosquitto.conf` | Broker & record service | Broker, open on the LAN, persistence on. |
| `service/record_service.py` | Broker & record service | Stores readings, detects holes, duplicates and clock reversals, prints the certificate. |
| `app/dashboard.py` | Application & evaluation | Live terminal view of the record strip. |
| `tools/fake_node.py` | all | **Stub node.** Lets the three laptop programs be built and tested before the Arduino exists. |

## Running it

```bash
# 1. broker
mosquitto -c broker/mosquitto.conf -v

# 2. the service that judges the record
python3 service/record_service.py --broker localhost --device node1

# 3. the live view, in another terminal
python3 app/dashboard.py --broker localhost --device node1

# 4. no hardware yet? drive it with the stub:
python3 tools/fake_node.py --outage 20:40 --excursion 55:60
#    and to prove the service really detects a hole:
python3 tools/fake_node.py --sid S9002 --drop 12,13
```

Then ask for the verdict:

```bash
python3 service/record_service.py --certificate S9001
```

Dependencies: `pip install paho-mqtt`, and `mosquitto` from the distribution.

## What the node guarantees

1. A reading is written to data flash **before** any attempt to publish it.
2. `ackedSeq` only advances once the broker has acknowledged (QoS 1).
3. The header is stored in two alternating copies with a CRC, so a power cut
   during a header write always leaves one readable.
4. If the ring overwrites a record that was never delivered, the node says so
   (`buffer_overrun`) instead of skipping it quietly. A hole the service can
   see is far better than a silent gap.
5. Timestamps come from the RTC, resynchronised from the network on every
   reconnection, because the millisecond counter restarts at zero on a reboot.

## Not verified yet — do these first

- [ ] **Does the board join the campus WiFi?** The biggest risk in the project.
      WPA2-Enterprise and captive portals are likely to refuse it. Fallback:
      a phone hotspot with the broker on the laptop beside it. Test before
      anything else is built on this architecture.
- [ ] **Which DHT is in the kit** — `DHT11` (±2 °C) or `DHT22` (±0.5 °C). Set
      `DHT_TYPE` in `config.h`. It changes nothing in the architecture and
      everything in how strongly the report may describe what is certified.
- [ ] **Data flash write time and endurance on the R4.** `EEPROM.put()` is
      backed by flash here, not real EEPROM. Measure one write, and check the
      cycle rating before settling on `SAMPLE_PERIOD_S` and `BUFFER_SLOTS`.
- [ ] **`WiFi.getTime()`** — confirm it returns a sane epoch on this core. If
      not, replace `syncClock()` with an NTP request; everything else is
      written against `nowEpoch()` and does not care.
- [ ] The firmware has **not been compiled** — there is no Arduino toolchain
      on the machine it was written on. Expect the first build to need small
      fixes, most likely around the `RTC`, `EEPROM` and LED-matrix APIs.

The three Python programs do compile, and the certificate logic is tested
against five cases: a clean shipment, a hole, a harmless identical duplicate,
a contradicting duplicate, and a clock reversal.
