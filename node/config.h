#pragma once
/* ============================================================
 *  Everything the team needs to change lives in this file.
 *  node.ino should not contain a single tunable constant.
 * ============================================================ */

/* ---- identity ------------------------------------------- */
#define DEVICE_ID      "node1"
#define TOPIC_ROOT     "coldchain/" DEVICE_ID

/* ---- network -------------------------------------------- */
// A phone hotspot is the fallback if the campus network refuses the board.
#define WIFI_SSID      "CHANGE_ME"
#define WIFI_PASS      "CHANGE_ME"
#define BROKER_HOST    "192.168.1.10"     // the laptop running mosquitto
#define BROKER_PORT    1883
#define MQTT_KEEPALIVE 10                 // seconds; the broker declares the
                                          // node dead after 1.5x this, so the
                                          // Last Will surfaces in about 15 s

/* ---- recording ------------------------------------------ */
#define SAMPLE_PERIOD_S   60              // one reading per minute
#define BUFFER_SLOTS     700              // 700 x 10 B = 7000 B of data flash
                                          // 700 slots x 60 s = ~11.6 h offline
#define TEMP_MIN_C       2.0f             // lower edge of the accepted band
#define TEMP_MAX_MIN_C   4.0f             // pot fully left
#define TEMP_MAX_MAX_C  30.0f             // pot fully right (so the room itself
                                          // can be made an excursion, for the demo)

/* ---- sensor --------------------------------------------- */
// The kit's module is a DHT. Set the right one: DHT11 reads +-2 C, DHT22 +-0.5 C.
// If a DS18B20 probe is bought instead, replace readSensor() in node.ino —
// the rest of the firmware does not care where the number comes from.
#define DHT_TYPE       DHT11              // or DHT22
#define DHT_MIN_PERIOD_MS 2000            // a DHT11 needs >= 1 s between reads

/* ---- pins ----------------------------------------------- */
#define PIN_DHT         6
#define PIN_US_TRIG     4                 // lid detection (stretch goal)
#define PIN_US_ECHO     5
#define PIN_POT        A0
#define PIN_BUTTON     A1
#define PIN_LED_OK      7                 // green  - in band, synchronised
#define PIN_LED_BUF     8                 // yellow - offline, buffering
#define PIN_LED_ALARM   9                 // red    - excursion, or buffer full
#define PIN_BUZZER      3

/* ---- features ------------------------------------------- */
#define USE_LID_SENSOR   0                // 1 once the ultrasonic is wired
#define LID_OPEN_CM     15                // beyond this distance the lid is open
#define BUZZER_ON_EXCURSION 1

/* ---- demo ----------------------------------------------- */
// Shortens the sampling period so a demo does not take an hour.
// Set to 0 for a real run.
#define DEMO_PERIOD_S    2
