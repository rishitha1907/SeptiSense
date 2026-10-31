/*
  SeptiSense - Embedded monitoring node (ESP8266 NodeMCU, ESP-12E)
  Educational / research prototype. NOT a clinically validated device.

  Version 1: sensors + real-time Wi-Fi telemetry only
             (LEDs / buzzer / button will be added later)

  Sensors : AD8232 (ECG on A0), MAX30102 (HR + SpO2), MLX90614 (temp), SSD1306 OLED
  Network : Wi-Fi -> HTTP POST JSON to laptop every 1 s
            (offline buffer of 20 s if Wi-Fi / server is down)

  Board   : "NodeMCU 1.0 (ESP-12E Module)"  (esp8266 by ESP8266 Community)
            Tools -> CPU Frequency -> 160 MHz
  Libraries (Library Manager):
    - Adafruit SSD1306 (+ Adafruit GFX)
    - Adafruit MLX90614 Library
    - SparkFun MAX3010x Pulse and Proximity Sensor Library
*/

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <Wire.h>
#include <time.h>
#include <sys/time.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_MLX90614.h>
#include "MAX30105.h"
#include "spo2_algorithm.h"

// ===================== USER SETTINGS (EDIT THESE) =====================
const char* WIFI_SSID  = "Dhanvitha's S23 FE";
const char* WIFI_PASS  = "anvitha sankuratri";
const char* SERVER_URL = "http://10.227.182.225:5000/api/telemetry"; // laptop IP
const char* DEVICE_ID  = "septisense-node-01";
const char* PATIENT_ID = "demo-patient-01";

#define SWAP_RED_IR    0      // set to 1 if SpO2 is always invalid / unrealistic
#define TEMP_OFFSET_C  0.0f   // calibrate MLX90614 skin reading if needed

// Demo alert thresholds (computed and sent to backend; NOT clinical)
#define HR_HIGH     100.0f
#define HR_LOW       50.0f
#define SPO2_LOW     92.0f
#define TEMP_HIGH    38.0f
#define TEMP_LOW     33.0f
#define ALERT_PERSIST_S  3

// ===================== PINS (NodeMCU label = GPIO) =====================
#define PIN_SDA        4    // D2
#define PIN_SCL        5    // D1
#define PIN_LO_PLUS   14    // D5
#define PIN_LO_MINUS  12    // D6
#define PIN_ECG       A0    // AD8232 OUTPUT

// ===================== CONSTANTS =====================
#define ECG_FS           125                 // ECG samples per second
#define ECG_PERIOD_US    (1000000UL / ECG_FS)
#define ECG_MAX_PKT      (ECG_FS + 10)
#define BUF_PKTS         20                  // 20 s offline buffer
#define FINGER_IR_MIN    50000
#define PPG_BUF_LEN      100                 // 4 s at 25 samples/s
#define PPG_STEP         25

#define ALERT_HR    0x01
#define ALERT_SPO2  0x02
#define ALERT_TEMP  0x04

// ===================== OBJECTS =====================
Adafruit_SSD1306 display(128, 64, &Wire, -1);
Adafruit_MLX90614 mlx;
MAX30105 maxSensor;
WiFiClient wifiClient;

bool oledOk = false, maxOk = false, mlxOk = false;

// ===================== PACKET RING BUFFER =====================
struct Packet {
  unsigned long long tsMs;
  bool tsSynced;
  uint32_t seq;
  float hrPpg, spo2, hrEcg, tempObj, tempAmb;
  bool fingerOn, leadsOff, tempValid, maxOk, mlxOk, alert;
  uint8_t alertFlags;
  uint32_t dropped;
  uint16_t gapSamples;
  uint16_t ecgN;
  uint16_t ecg[ECG_MAX_PKT];
};

Packet ring[BUF_PKTS];
int rHead = 0, rTail = 0, rCount = 0;
uint32_t droppedPackets = 0;

// ===================== STATE =====================
float hrPpg = NAN, spo2 = NAN, hrEcg = NAN, tempObj = NAN, tempAmb = NAN;
bool fingerOn = false, tempValid = false, leadsOff = true;
uint32_t lastPpgValidMs = 0;

uint16_t ecgPkt[ECG_MAX_PKT];
uint16_t ecgCount = 0, gapSamples = 0, lastEcg = 512;
uint32_t nextEcgUs = 0;

uint32_t irBuf[PPG_BUF_LEN], redBuf[PPG_BUF_LEN];
int ppgIdx = 0;
uint32_t lastIr = 0, lastRed = 0;

bool alertActive = false;
uint8_t alertFlags = 0;
int alertSeconds = 0;

bool wifiOk = false, postOk = false, ntpStarted = false;
uint32_t lastSendTry = 0;
uint32_t seq = 0;

// ===================== TIME =====================
unsigned long long nowEpochMs(bool &synced) {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  synced = tv.tv_sec > 1700000000;
  if (synced) return (unsigned long long)tv.tv_sec * 1000ULL + tv.tv_usec / 1000;
  return (unsigned long long)millis();
}

// ===================== ECG =====================
void processEcgSample(uint16_t v) {
  static float maxv = 512, minv = 512;
  static bool above = false;
  static uint32_t idx = 0, lastBeat = 0;
  static float rr[5];
  static int rrN = 0, rrPos = 0;

  idx++;
  if (leadsOff) { hrEcg = NAN; rrN = 0; above = false; lastBeat = 0; return; }

  float span = maxv - minv;
  if (v > maxv) maxv = v; else maxv -= span * 0.003f;
  if (v < minv) minv = v; else minv += span * 0.003f;
  span = maxv - minv;

  if (lastBeat && idx - lastBeat > 3 * ECG_FS) { hrEcg = NAN; rrN = 0; }
  if (span < 40) return;                       // flat / too small (10-bit ADC)

  float thr = minv + 0.65f * span;
  if (!above && v > thr) {
    above = true;
    uint32_t dt = idx - lastBeat;
    if (dt > ECG_FS * 0.3f) {                   // 300 ms refractory
      if (lastBeat && dt < ECG_FS * 2) {
        rr[rrPos] = dt; rrPos = (rrPos + 1) % 5; if (rrN < 5) rrN++;
        float s = 0; for (int i = 0; i < rrN; i++) s += rr[i];
        hrEcg = 60.0f * ECG_FS / (s / rrN);
      }
      lastBeat = idx;
    }
  } else if (above && v < thr) {
    above = false;
  }
}

// Call this as often as possible. Samples on a fixed 125 Hz grid.
// If the loop was blocked, missed slots are filled with the last value
// and counted in gapSamples (sent as a data-quality metric).
void serviceEcg() {
  uint32_t now = micros();
  int guard = 0;
  while ((int32_t)(now - nextEcgUs) >= 0 && guard < 250) {
    uint16_t v;
    bool late = (int32_t)(now - nextEcgUs) >= (int32_t)ECG_PERIOD_US;
    if (late) {
      v = lastEcg;
      gapSamples++;
    } else {
      leadsOff = digitalRead(PIN_LO_PLUS) || digitalRead(PIN_LO_MINUS);
      v = analogRead(PIN_ECG);
      lastEcg = v;
    }
    if (ecgCount < ECG_MAX_PKT) ecgPkt[ecgCount++] = v;
    processEcgSample(v);
    nextEcgUs += ECG_PERIOD_US;
    guard++;
  }
  if (guard >= 250) nextEcgUs = micros() + ECG_PERIOD_US;   // resync after long stall
}

// ===================== MAX30102 =====================
void pollMax() {
  if (!maxOk) return;
  maxSensor.check();
  while (maxSensor.available()) {
    uint32_t red = maxSensor.getFIFORed();
    uint32_t ir  = maxSensor.getFIFOIR();
    maxSensor.nextSample();
#if SWAP_RED_IR
    uint32_t t = red; red = ir; ir = t;
#endif
    lastIr = ir; lastRed = red;
    if (ir < FINGER_IR_MIN) {
      fingerOn = false; ppgIdx = 0; hrPpg = NAN; spo2 = NAN;
      continue;
    }
    fingerOn = true;
    irBuf[ppgIdx] = ir; redBuf[ppgIdx] = red; ppgIdx++;

    if (ppgIdx == PPG_BUF_LEN) {
      serviceEcg();
      int32_t s, h; int8_t sv, hv;
      maxim_heart_rate_and_oxygen_saturation(irBuf, PPG_BUF_LEN, redBuf, &s, &sv, &h, &hv);
      if (hv && h >= 40 && h <= 180) {
        hrPpg = isnan(hrPpg) ? h : 0.7f * hrPpg + 0.3f * h;
        lastPpgValidMs = millis();
      }
      if (sv && s >= 80 && s <= 100) {
        spo2 = isnan(spo2) ? s : 0.7f * spo2 + 0.3f * s;
        lastPpgValidMs = millis();
      }
      memmove(irBuf,  irBuf  + PPG_STEP, (PPG_BUF_LEN - PPG_STEP) * sizeof(uint32_t));
      memmove(redBuf, redBuf + PPG_STEP, (PPG_BUF_LEN - PPG_STEP) * sizeof(uint32_t));
      ppgIdx = PPG_BUF_LEN - PPG_STEP;
      serviceEcg();
    }
  }
  if (millis() - lastPpgValidMs > 10000) { hrPpg = NAN; spo2 = NAN; }
}

// ===================== MLX90614 =====================
void readTemp() {
  if (!mlxOk) { tempValid = false; tempObj = NAN; tempAmb = NAN; return; }
  float o = mlx.readObjectTempC();
  float a = mlx.readAmbientTempC();
  tempAmb = a;
  if (isnan(o)) { tempValid = false; tempObj = NAN; return; }
  o += TEMP_OFFSET_C;
  tempValid = (o >= 30.0f && o <= 43.0f);
  tempObj = tempValid ? o : NAN;
}

// ===================== ALERT FLAGS (sent to backend) =====================
void updateAlerts() {
  uint8_t f = 0;
  float hr = !isnan(hrPpg) ? hrPpg : hrEcg;
  if (!isnan(hr) && (hr > HR_HIGH || hr < HR_LOW)) f |= ALERT_HR;
  if (!isnan(spo2) && spo2 < SPO2_LOW) f |= ALERT_SPO2;
  if (tempValid && (tempObj > TEMP_HIGH || tempObj < TEMP_LOW)) f |= ALERT_TEMP;
  if (f) alertSeconds++; else alertSeconds = 0;
  alertActive = alertSeconds >= ALERT_PERSIST_S;
  alertFlags = alertActive ? f : 0;
}

// ===================== OLED (text only, 1x per second) =====================
void fmt(char *out, size_t n, float v, int dec) {
  if (isnan(v)) snprintf(out, n, "--");
  else snprintf(out, n, "%.*f", dec, v);
}

void drawOled() {
  if (!oledOk) return;
  char a[12], b[12], c[12], d[12];
  fmt(a, sizeof(a), hrPpg, 0);
  fmt(b, sizeof(b), spo2, 0);
  fmt(c, sizeof(c), tempObj, 1);
  fmt(d, sizeof(d), hrEcg, 0);

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);  display.printf("HR   %s bpm", a);
  display.setCursor(0, 12); display.printf("SpO2 %s %%", b);
  display.setCursor(0, 24); display.printf("Temp %s C", c);
  display.setCursor(0, 36); display.printf("ECG  %s", leadsOff ? "LEADS OFF" : d);
  display.setCursor(0, 52);
  display.print(wifiOk ? (postOk ? "WiFi OK" : "No server") : "WiFi offline");
  display.setCursor(90, 52); display.printf("Q:%d", rCount);

  Wire.setClock(400000);     // send frame fast (shorter ECG gap)
  display.display();
  Wire.setClock(100000);     // back to 100 kHz for MLX90614
}

// ===================== PACKET / JSON =====================
void makePacket() {
  if (rCount == BUF_PKTS) {                   // buffer full: drop oldest
    rTail = (rTail + 1) % BUF_PKTS;
    rCount--;
    droppedPackets++;
  }
  Packet &p = ring[rHead];
  bool synced;
  p.tsMs = nowEpochMs(synced);
  p.tsSynced = synced;
  p.seq = seq++;
  p.hrPpg = hrPpg; p.spo2 = spo2; p.hrEcg = hrEcg;
  p.tempObj = tempObj; p.tempAmb = tempAmb;
  p.fingerOn = fingerOn; p.leadsOff = leadsOff; p.tempValid = tempValid;
  p.maxOk = maxOk; p.mlxOk = mlxOk;
  p.alert = alertActive; p.alertFlags = alertFlags;
  p.dropped = droppedPackets;
  p.gapSamples = gapSamples;
  p.ecgN = ecgCount;
  memcpy(p.ecg, ecgPkt, ecgCount * sizeof(uint16_t));
  ecgCount = 0;
  gapSamples = 0;
  rHead = (rHead + 1) % BUF_PKTS;
  rCount++;
}

const char *jf(float v, char *buf, size_t n, int dec) {
  if (isnan(v)) snprintf(buf, n, "null"); else snprintf(buf, n, "%.*f", dec, v);
  return buf;
}
const char *jb(bool b) { return b ? "true" : "false"; }

static char json[1800];

void buildJson(const Packet &p) {
  char f1[12], f2[12], f3[12], f4[12], f5[12];
  size_t n = sizeof(json);
  int len = snprintf(json, n,
    "{\"device_id\":\"%s\",\"patient_id\":\"%s\",\"seq\":%lu,"
    "\"ts_ms\":%llu,\"ts_synced\":%s,"
    "\"hr_ppg\":%s,\"spo2\":%s,\"hr_ecg\":%s,\"temp_obj_c\":%s,\"temp_amb_c\":%s,"
    "\"quality\":{\"finger_on\":%s,\"ecg_leads_off\":%s,\"temp_valid\":%s,"
    "\"max30102_ok\":%s,\"mlx90614_ok\":%s,\"ecg_gap_samples\":%u},"
    "\"alert\":%s,\"alert_flags\":%u,\"dropped\":%lu,\"ecg_fs\":%d,\"ecg\":[",
    DEVICE_ID, PATIENT_ID, (unsigned long)p.seq,
    p.tsMs, jb(p.tsSynced),
    jf(p.hrPpg, f1, 12, 1), jf(p.spo2, f2, 12, 1), jf(p.hrEcg, f3, 12, 1),
    jf(p.tempObj, f4, 12, 2), jf(p.tempAmb, f5, 12, 2),
    jb(p.fingerOn), jb(p.leadsOff), jb(p.tempValid), jb(p.maxOk), jb(p.mlxOk),
    (unsigned)p.gapSamples,
    jb(p.alert), (unsigned)p.alertFlags, (unsigned long)p.dropped, ECG_FS);
  for (int i = 0; i < p.ecgN && len < (int)n - 10; i++)
    len += snprintf(json + len, n - len, i ? ",%u" : "%u", p.ecg[i]);
  snprintf(json + len, n - len, "]}");
}

// ===================== NETWORK =====================
void serviceNetwork() {
  wifiOk = (WiFi.status() == WL_CONNECTED);
  if (!wifiOk) { postOk = false; return; }

  if (!ntpStarted) {
    Serial.print("WiFi connected, IP = ");
    Serial.println(WiFi.localIP());
    configTime(0, 0, "pool.ntp.org", "time.google.com");   // UTC epoch
    ntpStarted = true;
  }

  if (rCount == 0) return;
  if (!postOk && millis() - lastSendTry < 3000) return;    // back off when offline
  lastSendTry = millis();

  serviceEcg();
  buildJson(ring[rTail]);
  serviceEcg();

  HTTPClient http;
  http.setTimeout(800);
  if (!http.begin(wifiClient, SERVER_URL)) { postOk = false; return; }
  http.addHeader("Content-Type", "application/json");
  int code = http.POST((uint8_t *)json, strlen(json));
  http.end();
  serviceEcg();

  if (code >= 200 && code < 300) {
    postOk = true;
    rTail = (rTail + 1) % BUF_PKTS;
    rCount--;
  } else {
    postOk = false;
    Serial.printf("HTTP POST failed (code %d) - buffering\n", code);
  }
}

// ===================== SETUP =====================
void i2cScan() {
  Serial.println("I2C scan:");
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) Serial.printf("  found 0x%02X\n", a);
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== SeptiSense ESP8266 node starting ===");

  pinMode(PIN_LO_PLUS, INPUT);
  pinMode(PIN_LO_MINUS, INPUT);

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(100000);
  i2cScan();

  oledOk = display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  if (oledOk) {
    display.clearDisplay(); display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0); display.println("SeptiSense");
    display.println("starting...");
    display.display();
  }

  maxOk = maxSensor.begin(Wire, I2C_SPEED_STANDARD);
  if (maxOk) maxSensor.setup(60, 4, 2, 100, 411, 4096);   // 25 samples/s output

  mlxOk = mlx.begin();
  Wire.setClock(100000);

  Serial.printf("OLED: %s  MAX30102: %s  MLX90614: %s\n",
                oledOk ? "OK" : "MISSING", maxOk ? "OK" : "MISSING", mlxOk ? "OK" : "MISSING");

  

  nextEcgUs = micros() + ECG_PERIOD_US;
}

// ===================== LOOP =====================
void loop() {
  static uint32_t lastSec = 0;
  uint32_t now = millis();

  serviceEcg();
  pollMax();
  serviceEcg();

  if (now - lastSec >= 1000) {
    lastSec = now;
    readTemp();
    serviceEcg();
    updateAlerts();
    makePacket();
    Serial.printf("[%lu] HR_ppg=%.0f SpO2=%.0f HR_ecg=%.0f Temp=%.2fC Amb=%.2fC "
                  "IR=%lu RED=%lu ECGraw=%u finger=%d leadsOff=%d wifi=%d server=%d queued=%d\n",
                  (unsigned long)(seq - 1), hrPpg, spo2, hrEcg,  tempAmb,
                  (unsigned long)lastIr, (unsigned long)lastRed, lastEcg,
                  fingerOn, leadsOff, wifiOk, postOk, rCount);
    serviceEcg();
    drawOled();
    serviceEcg();
  }

  serviceNetwork();
  serviceEcg();
  yield();
}
