# Project context — cold chain logger

*Attach this file at the start of a Claude conversation and it has everything
it needs to help on this project. Attach the source files too if the question
is about code.*

## What we are building

A temperature logger for a cold-chain shipment (vaccines, blood products,
fresh food) whose record can be **proven complete**.

Measuring temperature is trivial. The hard part — and the whole project — is
proving that **no measurement is missing**, across a journey where the network
comes and goes and the logger can reboot. A shipment is released only against
a gap-free record; a hole sends it into quarantine and assessment.

This is a team project for a university IoT course. The rule is that the topic
must be about communication / network. One Arduino board, four people,
10-minute presentation.

**This is a demonstrator of the data path, not a medical device.** The kit's
DHT11 reads to about ±2 °C and the vaccine band is 2–8 °C, so the sensor
cannot police it. Say so rather than overclaiming.

## Architecture

```
[Arduino Uno R4 WiFi] --WiFi/TCP/MQTT--> [mosquitto] --> record_service.py
      node.ino                            on a laptop  \-> dashboard.py
```

No gateway: the R4 WiFi is a network client in its own right.

**The central design argument.** WiFi, TCP and MQTT already give checksums,
acknowledgement and retransmission. We do not reimplement any of that. TCP
guarantees delivery only while the connection lives — it says nothing across a
disconnection, nothing across a reboot, and nothing about what was stored.
Completeness is an end-to-end property, so it is enforced end to end: sequence
numbers, non-volatile buffering, ordered replay, deduplication. (This is the
end-to-end argument, Saltzer, Reed & Clark, 1984.)

## The contract

`docs/topics.md` is frozen. Change it there before touching any program.

Topics, all QoS 1 (never 0 — a lost reading is a hole):

| Topic | Retained | Meaning |
|---|---|---|
| `coldchain/<dev>/status` | yes | `online` / `offline`; the broker publishes `offline` itself via the Last Will |
| `coldchain/<dev>/shipment` | yes | current shipment id and band |
| `coldchain/<dev>/reading` | no | one reading — the sequence of these is what completeness is checked on |
| `coldchain/<dev>/event` | no | boot, shipment start, excursion in/out, lid open, buffer overrun |

Reading payload:

```json
{"sid":"S0007","seq":142,"ts":1760000000,"t":4.21,"h":48,"f":"B"}
```

`seq` starts at 0 for each shipment, never skipped, never reused. `f` flags:
`""` live, `"B"` replayed from the buffer, `"X"` outside the band.

QoS 1 is at-least-once, so duplicates are expected after a reconnection. The
service deduplicates on `seq` — that is how the chain reaches exactly-once
semantics without paying for QoS 2 on a constrained node. **A duplicate
carrying different content invalidates the record**, as badly as a hole.

## The four rules the record service enforces

1. No hole: `seq` must be `0..max` with nothing missing.
2. No contradicting duplicate.
3. Time must not go backwards inside a shipment.
4. No silent death: `status` must not be `offline` with readings still expected.

## Files

| Path | Owner | What it is |
|---|---|---|
| `docs/topics.md` | all | the frozen contract |
| `node/node.ino`, `node/config.h` | Node & logging | firmware; every tunable is in `config.h` |
| `broker/mosquitto.conf` | Broker & record service | broker on the LAN, persistence on |
| `service/record_service.py` | Broker & record service | SQLite store, hole/duplicate/clock detection, `--certificate SID` |
| `app/dashboard.py` | Application & evaluation | live colour strip of the record |
| `tools/fake_node.py` | all | stub node, so the laptop side can be built without hardware |

## Conventions that matter

- **No `String` objects on the Arduino.** They allocate on a heap that
  fragments; in the companion project the receiver stopped answering after
  ~65 frames because of exactly this. Fixed `char` buffers, literals in `F()`.
- **A reading is written to data flash *before* any attempt to publish it.**
  `ackedSeq` only advances once the broker has acknowledged.
- **The header is kept in two alternating CRC'd copies**, so a power cut during
  a header write always leaves one readable.
- **A ring overrun is announced** (`buffer_overrun` event), never skipped. A
  hole the service can see beats a silent gap.
- **Timestamps come from the RTC**, resynchronised from the network on every
  reconnection — the millisecond counter restarts at zero on a reboot.

## Scope

**Must have:** fixed-cadence sampling, sequence numbers, non-volatile ring
buffer, reconnect and replay in order, deduplication, heartbeat and Last Will,
a dashboard whose timeline is visibly gap-free.

**Stretch, in this order:** resumable backlog upload after an interruption,
running integrity hash over the whole record, chunk-size study, lid-open
events in the same sequence.

Ten minutes is not long, and a demo that half-works scores worse than a
smaller one that works.

## The demo

1. Carry the node out of WiFi range on a power bank — yellow LED, the LED
   matrix fills with buffered readings.
2. Breathe on the sensor while outside — the excursion is recorded with no
   network at all.
3. Come back — the backlog uploads in order, the dashboard fills in the
   missing minutes, the sequence is unbroken.
4. Cut the power entirely — RAM is wiped, but the readings were in data flash,
   so the node resumes. *This is the step that separates a buffer from a record.*

## Status

**Done:** all four programs written. The three Python programs compile, and
the certificate logic is tested against five cases — clean shipment, hole,
identical duplicate (harmless), contradicting duplicate, clock reversal.

**Not done / not verified:**

- The firmware has **never been compiled** — expect first-build fixes, most
  likely around the `RTC`, `EEPROM` and LED-matrix APIs.
- **Does the board join the campus WiFi?** The biggest risk in the project.
  WPA2-Enterprise and captive portals are likely to refuse it. Fallback: a
  phone hotspot with the broker on the laptop beside it. Test this first.
- Which DHT is in the kit — `DHT11` (±2 °C) or `DHT22` (±0.5 °C).
- R4 data-flash write time and endurance, which bound `SAMPLE_PERIOD_S` and
  `BUFFER_SLOTS`.
- `WiFi.getTime()` behaviour on this core.
- Report sections 5, 6 and 7, which depend on the measurement campaign.
- Team name, member names and IDs, contribution percentages, signatures.

## Measurements planned for the report

1. Record completeness — zero holes, zero duplicates, over several hundred
   readings with induced outages.
2. Offline endurance — buffer slots × sampling interval, designed then verified.
3. Reconnection and drain time, for several backlog sizes.
4. Dead-node detection time — MQTT drops a silent client after 1.5 × keep-alive,
   so a 10 s keep-alive should surface the Last Will in about 15 s.
5. *(stretch)* resume cost after cutting an upload at 25, 50, 75 %.

Note that the report template allows only 5–10 lines for the results section,
so only two or three of these fit in writing; the rest go on slides.
