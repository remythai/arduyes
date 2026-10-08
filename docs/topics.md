# MQTT contract

Frozen interface between the node, the record service and the dashboard.
Nothing in the three programs may diverge from this file; change it here first.

`<dev>` is the device id, e.g. `node1`.

## Topics

| Topic | QoS | Retained | Published by | Meaning |
|---|---|---|---|---|
| `coldchain/<dev>/status` | 1 | yes | node, and the broker | `online` / `offline`. The broker publishes `offline` itself through the Last Will when the node disappears without disconnecting. |
| `coldchain/<dev>/shipment` | 1 | yes | node | The shipment currently being recorded. A late subscriber learns the context immediately. |
| `coldchain/<dev>/reading` | 1 | no | node | One temperature reading. The sequence of these is what completeness is checked on. |
| `coldchain/<dev>/event` | 1 | no | node | Boot, shipment start, excursion entered or left, lid opened, buffer nearly full. |

QoS 1 everywhere, never 0: a lost reading is a hole in the record.
QoS 1 can duplicate, which is why every payload carries `seq` and the service
deduplicates on it — that is how the chain reaches exactly-once semantics
without paying for QoS 2 on a constrained node.

## Payloads

All payloads are compact JSON, one line, no spaces. Unknown fields must be
ignored by readers so the node can add some later without breaking anything.

### `reading`

```json
{"sid":"S0007","seq":142,"ts":1760000000,"t":4.21,"h":48,"f":"B"}
```

| Field | Type | Meaning |
|---|---|---|
| `sid` | string | Shipment id. `seq` is only meaningful inside one shipment. |
| `seq` | int | Sequence number, starts at 0 for each shipment, never reused, never skipped. |
| `ts` | int | Unix seconds, from the node's RTC. |
| `t` | float | Temperature in °C, 2 decimals. |
| `h` | int | Relative humidity in %. `-1` when the sensor has no humidity channel. |
| `f` | string | Flags. `""` published live, `"B"` replayed from the buffer, `"X"` outside the band, `"BX"` both. |

### `event`

```json
{"sid":"S0007","seq":142,"ts":1760000000,"e":"excursion_in","v":9.4}
```

`e` is one of `boot`, `shipment_start`, `excursion_in`, `excursion_out`,
`lid_open`, `buffer_warn`. `v` is an optional numeric detail. `seq` is the
reading the event is attached to, so the event sits in the same ordered chain.

### `shipment` (retained)

```json
{"sid":"S0007","ts":1760000000,"tmin":2.0,"tmax":8.0,"period":60}
```

### `status` (retained)

The string `online`, or `offline` — the latter published by the broker as the
node's Last Will.

## What the record service checks

1. **No hole.** For a shipment, the set of `seq` received must be
   `0..max` with nothing missing. A hole is the only fatal defect.
2. **No duplicate with different content.** The same `seq` arriving twice is
   normal after a reconnection; the second copy must be byte-identical to the
   first. A contradiction invalidates the record.
3. **Monotonic time.** `ts` must not go backwards within a shipment. It can
   jump forward after a reboot, which is why the node resynchronises its clock
   before publishing anything.
4. **No silent death.** `status` must be `online`. An `offline` with readings
   still expected means the record stops there and the shipment is incomplete.

A shipment is *certifiable* when 1, 2 and 3 hold and the node is still
`online` or finished cleanly.
