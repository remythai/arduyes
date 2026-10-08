/* ============================================================
 *  COLD CHAIN NODE — Arduino Uno R4 WiFi
 *
 *  Samples temperature on a fixed cadence, writes every reading to data
 *  flash BEFORE trying to publish it, and only advances its "delivered"
 *  pointer once the broker has acknowledged. The record therefore survives
 *  a dropped connection, a reboot, and a power cut.
 *
 *  What this firmware does NOT do, on purpose:
 *  WiFi, TCP and MQTT already provide checksums, acknowledgement and
 *  retransmission. None of that survives a disconnection or a reboot, and
 *  none of it proves anything was stored. So the only reliability logic
 *  here is the part TCP cannot give us — sequence numbers, non-volatile
 *  buffering and ordered replay. Everything else is left to the stack.
 *
 *  Storage layout in the 8 KB data flash (see docs/topics.md):
 *      0   .. 31    header copy A
 *      32  .. 63    header copy B          (alternating, for atomicity)
 *      64  .. end   ring of BUFFER_SLOTS records, 10 bytes each
 *
 *  No String objects anywhere: they allocate on the heap and fragment it.
 *  Fixed char buffers only, literals in F().
 * ============================================================ */
#include <WiFiS3.h>
#include <ArduinoMqttClient.h>
#include <EEPROM.h>
#include <RTC.h>
#include <DHT.h>
#include "Arduino_LED_Matrix.h"
#include "config.h"

/* ---------- storage types ---------- */
struct Record {               // 10 bytes, packed by hand to keep the maths obvious
  uint16_t seq;
  uint32_t ts;
  int16_t  t_centi;           // temperature x100, so 4.21 C -> 421
  uint8_t  rh;                // 0..100, or 255 when the sensor has no humidity
  uint8_t  flags;             // bit0 excursion, bit1 replayed
};
#define FLAG_EXCURSION 0x01
#define FLAG_REPLAYED  0x02

struct Header {
  uint32_t magic;             // 'C''C''0''1'
  uint32_t writeCount;        // the higher of the two valid copies wins
  char     sid[8];            // shipment id, e.g. "S0007"
  uint16_t nextSeq;           // sequence to give the next reading
  uint16_t head;              // ring slot the next record goes into
  uint16_t ackedSeq;          // highest seq the broker has acknowledged, +1
  uint16_t bootCount;
  uint32_t crc;               // over everything above
};

#define HDR_MAGIC    0x43433031UL
#define HDR_A_ADDR   0
#define HDR_B_ADDR   32
#define RING_ADDR    64
#define REC_SIZE     10

/* ---------- globals ---------- */
Header       hdr;
bool         hdrSlotB = false;        // which copy was written last
WiFiClient   net;
MqttClient   mqtt(net);
DHT          dht(PIN_DHT, DHT_TYPE);
ArduinoLEDMatrix matrix;

unsigned long lastSampleMs = 0;
unsigned long lastSensorMs = 0;
unsigned long lastDrainMs  = 0;
float    lastTemp = NAN;
int      lastRh   = -1;
bool     inExcursion = false;
bool     alarmMuted  = false;
bool     linkUp      = false;

char topicStatus[64], topicShipment[64], topicReading[64], topicEvent[64];
char buf[192];                         // every payload is built in here

/* ============================================================
 *  small helpers
 * ============================================================ */
static uint32_t crc32(const uint8_t *p, size_t n) {
  uint32_t c = 0xFFFFFFFFUL;
  while (n--) {
    c ^= *p++;
    for (uint8_t k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320UL & (-(int32_t)(c & 1)));
  }
  return ~c;
}

static uint32_t nowEpoch() {
  RTCTime t;
  RTC.getTime(t);
  return t.getUnixTime();
}

static uint16_t ringCount() {           // records written but not yet acknowledged
  return hdr.nextSeq - hdr.ackedSeq;
}

/* ============================================================
 *  header: two alternating copies so a power cut mid-write
 *  always leaves one intact and readable
 * ============================================================ */
static bool headerValid(const Header &h) {
  if (h.magic != HDR_MAGIC) return false;
  Header tmp = h; tmp.crc = 0;
  return crc32((const uint8_t *)&tmp, sizeof(tmp) - sizeof(uint32_t)) == h.crc;
}

static void headerSave() {
  hdr.writeCount++;
  hdr.crc = 0;
  Header tmp = hdr;
  hdr.crc = crc32((const uint8_t *)&tmp, sizeof(tmp) - sizeof(uint32_t));
  hdrSlotB = !hdrSlotB;                 // alternate, never overwrite the good one
  EEPROM.put(hdrSlotB ? HDR_B_ADDR : HDR_A_ADDR, hdr);
}

static void headerLoad() {
  Header a, b;
  EEPROM.get(HDR_A_ADDR, a);
  EEPROM.get(HDR_B_ADDR, b);
  bool va = headerValid(a), vb = headerValid(b);

  if (va && vb)      { hdr = (a.writeCount >= b.writeCount) ? a : b; hdrSlotB = !(a.writeCount >= b.writeCount); }
  else if (va)       { hdr = a; hdrSlotB = false; }
  else if (vb)       { hdr = b; hdrSlotB = true;  }
  else {                                        // first boot, or flash wiped
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = HDR_MAGIC;
    strcpy(hdr.sid, "S0000");
    Serial.println(F("# flash empty, starting a fresh header"));
  }
  hdr.bootCount++;
  headerSave();
}

/* ============================================================
 *  ring buffer in data flash
 *  A record is written the moment it is sampled. Publishing only moves
 *  ackedSeq forward — the data itself is never held in RAM alone.
 * ============================================================ */
static void ringWrite(const Record &r) {
  EEPROM.put(RING_ADDR + (uint32_t)hdr.head * REC_SIZE, r);
  hdr.head = (hdr.head + 1) % BUFFER_SLOTS;
}

static bool ringRead(uint16_t seq, Record &r) {
  if (seq < hdr.ackedSeq || seq >= hdr.nextSeq) return false;
  uint16_t behind = hdr.nextSeq - seq;                 // how far back from head
  if (behind > BUFFER_SLOTS) return false;             // overwritten, genuinely lost
  uint16_t slot = (hdr.head + BUFFER_SLOTS - behind) % BUFFER_SLOTS;
  EEPROM.get(RING_ADDR + (uint32_t)slot * REC_SIZE, r);
  return r.seq == seq;
}

/* ============================================================
 *  sensing
 * ============================================================ */
static void readSensor() {
  if (millis() - lastSensorMs < DHT_MIN_PERIOD_MS) return;
  lastSensorMs = millis();
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (!isnan(t)) lastTemp = t;
  if (!isnan(h)) lastRh = (int)(h + 0.5f);
}

static float bandMax() {                // the pot sets the upper edge of the band
  int raw = analogRead(PIN_POT);        // R4 ADC is 14-bit capable; default is 10
  return TEMP_MAX_MIN_C + (TEMP_MAX_MAX_C - TEMP_MAX_MIN_C) * (raw / 1023.0f);
}

#if USE_LID_SENSOR
static bool lidOpen() {
  digitalWrite(PIN_US_TRIG, LOW);  delayMicroseconds(2);
  digitalWrite(PIN_US_TRIG, HIGH); delayMicroseconds(10);
  digitalWrite(PIN_US_TRIG, LOW);
  unsigned long d = pulseIn(PIN_US_ECHO, HIGH, 30000UL);   // timeout, never block
  if (d == 0) return false;
  return (d / 58.0) > LID_OPEN_CM;
}
#endif

/* ============================================================
 *  publishing
 * ============================================================ */
static bool publish(const char *topic, const char *payload, bool retain) {
  if (!mqtt.connected()) return false;
  mqtt.beginMessage(topic, retain, 1);   // QoS 1: at-least-once, we dedup on seq
  mqtt.print(payload);
  return mqtt.endMessage() == 1;
}

static void publishEvent(const char *what, float v) {
  char vs[12];
  if (isnan(v)) strcpy(vs, "null"); else dtostrf(v, 1, 2, vs);
  snprintf(buf, sizeof(buf),
           "{\"sid\":\"%s\",\"seq\":%u,\"ts\":%lu,\"e\":\"%s\",\"v\":%s}",
           hdr.sid, (unsigned)hdr.nextSeq, (unsigned long)nowEpoch(), what, vs);
  publish(topicEvent, buf, false);
  Serial.print(F("# event ")); Serial.println(what);
}

static void publishShipment() {
  char lo[8], hi[8];
  dtostrf(TEMP_MIN_C, 1, 1, lo);
  dtostrf(bandMax(),  1, 1, hi);
  snprintf(buf, sizeof(buf),
           "{\"sid\":\"%s\",\"ts\":%lu,\"tmin\":%s,\"tmax\":%s,\"period\":%d}",
           hdr.sid, (unsigned long)nowEpoch(), lo, hi, SAMPLE_PERIOD_S);
  publish(topicShipment, buf, true);
}

// Builds the JSON for one stored record and sends it. Returns true on ACK.
static bool publishRecord(const Record &r, bool replayed) {
  char ts[10], hs[8];
  dtostrf(r.t_centi / 100.0f, 1, 2, ts);
  if (r.rh == 255) strcpy(hs, "-1"); else snprintf(hs, sizeof(hs), "%u", r.rh);
  char flags[4] = "";
  if (replayed || (r.flags & FLAG_REPLAYED))  strcat(flags, "B");
  if (r.flags & FLAG_EXCURSION)               strcat(flags, "X");
  snprintf(buf, sizeof(buf),
           "{\"sid\":\"%s\",\"seq\":%u,\"ts\":%lu,\"t\":%s,\"h\":%s,\"f\":\"%s\"}",
           hdr.sid, (unsigned)r.seq, (unsigned long)r.ts, ts, hs, flags);
  return publish(topicReading, buf, false);
}

/* Sends everything between ackedSeq and nextSeq, in order, stopping at the
   first failure so the record is never published out of order. */
static void drainBacklog() {
  uint8_t sent = 0;
  while (hdr.ackedSeq < hdr.nextSeq && sent < 20) {     // bounded, to keep the loop responsive
    Record r;
    if (!ringRead(hdr.ackedSeq, r)) {
      // the slot was overwritten: the record is genuinely gone. Say so loudly —
      // a hole the service can see is far better than a silent skip.
      publishEvent("buffer_overrun", (float)hdr.ackedSeq);
      Serial.print(F("# !! record lost, seq ")); Serial.println(hdr.ackedSeq);
      hdr.ackedSeq++;
      headerSave();
      continue;
    }
    bool replayed = (hdr.nextSeq - hdr.ackedSeq) > 1;   // not the one just sampled
    if (!publishRecord(r, replayed)) break;             // keep it for the next pass
    hdr.ackedSeq++;
    sent++;
  }
  if (sent) headerSave();
}

/* ============================================================
 *  connectivity
 * ============================================================ */
static bool connectAll() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.print(F("# wifi..."));
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) delay(250);
    if (WiFi.status() != WL_CONNECTED) { Serial.println(F(" failed")); return false; }
    Serial.println(F(" ok"));
  }

  if (!mqtt.connected()) {
    mqtt.setId(DEVICE_ID);
    mqtt.setKeepAliveInterval(MQTT_KEEPALIVE * 1000L);
    // The will is lodged BEFORE connecting: if the node dies, the broker says so.
    mqtt.beginWill(topicStatus, strlen("offline"), true, 1);
    mqtt.print("offline");
    mqtt.endWill();
    if (!mqtt.connect(BROKER_HOST, BROKER_PORT)) {
      Serial.print(F("# mqtt failed, err ")); Serial.println(mqtt.connectError());
      return false;
    }
    publish(topicStatus, "online", true);
    publishShipment();
    Serial.println(F("# mqtt connected"));
  }
  return true;
}

/* Clock: the millisecond counter restarts at zero on every boot, so a
   timestamp can only come from the RTC, resynchronised from the network. */
static void syncClock() {
  unsigned long epoch = WiFi.getTime();
  if (epoch > 1600000000UL) {            // sanity: after 2020
    RTCTime t(epoch);
    RTC.setTime(t);
    Serial.print(F("# clock synced, epoch ")); Serial.println(epoch);
  } else {
    Serial.println(F("# !! clock NOT synced - timestamps are unreliable"));
  }
}

/* ============================================================
 *  local interface
 * ============================================================ */
static void updateIndicators() {
  uint16_t pending = ringCount();
  digitalWrite(PIN_LED_OK,    linkUp && !inExcursion);
  digitalWrite(PIN_LED_BUF,   pending > 1);
  digitalWrite(PIN_LED_ALARM, inExcursion || pending > (BUFFER_SLOTS * 9 / 10));

  // the 12x8 matrix fills up as the backlog grows: the record strip, live
  uint8_t frame[8][12] = {0};
  uint16_t lit = (uint32_t)pending * 96 / BUFFER_SLOTS;
  for (uint16_t i = 0; i < lit && i < 96; i++) frame[i / 12][i % 12] = 1;
  matrix.renderBitmap(frame, 8, 12);
}

static void startShipment() {
  static uint16_t n = 1;
  snprintf(hdr.sid, sizeof(hdr.sid), "S%04u", n++);
  hdr.nextSeq = 0; hdr.ackedSeq = 0; hdr.head = 0;
  headerSave();
  publishShipment();
  publishEvent("shipment_start", NAN);
  Serial.print(F("# shipment ")); Serial.println(hdr.sid);
}

/* ============================================================
 *  setup / loop
 * ============================================================ */
void setup() {
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_LED_OK, OUTPUT); pinMode(PIN_LED_BUF, OUTPUT); pinMode(PIN_LED_ALARM, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
#if USE_LID_SENSOR
  pinMode(PIN_US_TRIG, OUTPUT); pinMode(PIN_US_ECHO, INPUT);
#endif

  Serial.begin(115200);
  delay(300);
  matrix.begin();
  dht.begin();
  RTC.begin();

  snprintf(topicStatus,   sizeof(topicStatus),   TOPIC_ROOT "/status");
  snprintf(topicShipment, sizeof(topicShipment), TOPIC_ROOT "/shipment");
  snprintf(topicReading,  sizeof(topicReading),  TOPIC_ROOT "/reading");
  snprintf(topicEvent,    sizeof(topicEvent),    TOPIC_ROOT "/event");

  headerLoad();
  Serial.print(F("# boot #"));      Serial.print(hdr.bootCount);
  Serial.print(F("  shipment "));   Serial.print(hdr.sid);
  Serial.print(F("  pending "));    Serial.println(ringCount());

  linkUp = connectAll();
  if (linkUp) { syncClock(); publishEvent("boot", (float)hdr.bootCount); }
}

void loop() {
  mqtt.poll();                                   // keeps the session and the will alive
  readSensor();

  /* --- button: short press mutes the alarm, long press starts a shipment --- */
  static unsigned long pressedAt = 0;
  if (digitalRead(PIN_BUTTON) == LOW) {
    if (!pressedAt) pressedAt = millis();
  } else if (pressedAt) {
    if (millis() - pressedAt > 1500) startShipment();
    else                             alarmMuted = true;
    pressedAt = 0;
  }

  /* --- sample on the cadence, store first, publish second --- */
  unsigned long periodMs = (DEMO_PERIOD_S ? DEMO_PERIOD_S : SAMPLE_PERIOD_S) * 1000UL;
  if (millis() - lastSampleMs >= periodMs) {
    lastSampleMs = millis();

    if (!isnan(lastTemp)) {
      bool out = (lastTemp < TEMP_MIN_C) || (lastTemp > bandMax());

      Record r;
      r.seq     = hdr.nextSeq;
      r.ts      = nowEpoch();
      r.t_centi = (int16_t)lroundf(lastTemp * 100.0f);
      r.rh      = (lastRh < 0) ? 255 : (uint8_t)lastRh;
      r.flags   = out ? FLAG_EXCURSION : 0;

      ringWrite(r);                 // non-volatile BEFORE any attempt to publish
      hdr.nextSeq++;
      headerSave();

      if (out != inExcursion) {
        inExcursion = out;
        alarmMuted = false;
        publishEvent(out ? "excursion_in" : "excursion_out", lastTemp);
      }
#if USE_LID_SENSOR
      static bool lastLid = false;
      bool lid = lidOpen();
      if (lid && !lastLid) publishEvent("lid_open", NAN);
      lastLid = lid;
#endif
    }
  }

  /* --- keep the link up and drain whatever is pending --- */
  if (millis() - lastDrainMs > 500) {
    lastDrainMs = millis();
    linkUp = connectAll();
    if (linkUp) {
      static uint16_t lastBoot = 0;
      if (lastBoot != hdr.bootCount) { syncClock(); lastBoot = hdr.bootCount; }
      drainBacklog();
    }
  }

#if BUZZER_ON_EXCURSION
  if (inExcursion && !alarmMuted) { tone(PIN_BUZZER, 2200, 120); }
#endif
  updateIndicators();
}
