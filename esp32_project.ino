/*
 * ESP32 -> RunCam Split 4 camera controller, driven from a phone over
 * Bluetooth LE (and optionally Wi-Fi).
 *
 * Board: ESP32 DevKit V1, WROOM-32 (esp32:esp32:esp32, PartitionScheme=huge_app)
 *
 * Bluetooth: the ESP32 advertises as "RunCam-Ctl" with the Nordic UART
 * Service. Connect with any BLE UART terminal app (e.g. Bluefruit Connect ->
 * UART) and type the same commands as the serial console -- send HELP.
 *
 * Wi-Fi (USE_WIFI = true): the ESP32 runs its own access point and serves a
 * control page. Join the network, open http://192.168.4.1 (or
 * http://runcam.local), and use the buttons.
 *
 * Camera states, as the page shows them:
 *   OFF        load switch open -- camera unpowered, zero draw
 *   BOOTING    switch closed, camera starting (~3 s)
 *   STANDBY    camera powered and ready, not recording
 *   RECORDING  recording to the camera's microSD card
 *   SAVING     stop sent; waiting for the camera to finalise the file
 *   STOPPING   saving, then the power will be cut
 *
 * The camera never gets its power cut while recording or saving: OFF always
 * stops recording first and waits for the file to close, because pulling
 * power mid-write is how microSD files get corrupted.
 *
 * Everything is also available as text commands on the USB serial console
 * (115200) -- send HELP.
 */

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLE2902.h>

#include "runcam.h"
#include "web_ui.h"

// ------------------------------------------------------------ Bluetooth -----
// BLE is the control link. Wi-Fi is off by default (saves battery); set
// USE_WIFI true to bring back the access point and web page alongside BLE.
static const bool  USE_WIFI = false;
static const char *BLE_NAME = "RunCam-Ctl";

// Nordic UART Service: phone writes commands to RX, ESP32 notifies on TX.
static const char *NUS_SERVICE = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
static const char *NUS_RX      = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";
static const char *NUS_TX      = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";

// ------------------------------------------------------------- Wi-Fi AP -----
// Defaults; change them here, or at runtime with the SSID / PASS commands or
// the page's Settings panel (saved to flash). WPA2 needs 8-63 characters.
static const char *DEFAULT_SSID = "RunCam-Ctl";
static const char *DEFAULT_PASS = "runcam1234";
static const char *MDNS_NAME    = "runcam";      // -> http://runcam.local
static const int   WIFI_CHANNEL = 6;

// ---------------------------------------------------------------- pins ------
// GPIO16/17 are free on WROOM-32 modules (this board is ESP32-D0WD-V3).
// On a WROVER module they are wired to PSRAM -- move the UART if you switch.
static const int PIN_CAM_RX  = 16;   // ESP32 RX  <- camera TX
static const int PIN_CAM_TX  = 17;   // ESP32 TX  -> camera RX
static const int PIN_CAM_PWR = 13;   // -> load-switch enable
static const int PIN_VBAT    = 34;   // ADC1_CH6, input-only; ADC1 works with Wi-Fi on
static const int PIN_LED     = 2;    // onboard LED

// Set false if your load switch enables when pulled low.
static const bool CAM_PWR_ACTIVE_HIGH = true;
// No load switch yet: GPIO13 does nothing and the camera stays powered from
// the 5 V rail, so OFF must not claim the camera is unpowered.
static const bool LOAD_SWITCH_FITTED = false;

// ------------------------------------------------------ battery divider -----
// VBAT --[R1]--+--[R2]-- GND, tap into PIN_VBAT.
// 100k/15k divides by 7.67, so a 20 V pack reads 2.61 V at the ADC pin.
static const float DIV_R1 = 100000.0f;
static const float DIV_R2 = 15000.0f;
static const float DIV_RATIO = (DIV_R1 + DIV_R2) / DIV_R2;

// No divider fitted yet: leave false and the pack is reported as "no battery
// sensor" instead of whatever a floating GPIO34 happens to read. Set true once
// R1/R2 are soldered in (for a 1S pack, 100k/100k gives far better resolution).
static const bool VBAT_SENSOR_FITTED = false;
// Even when fitted, a reading is only trusted if it looks like a real pack.
static const float VBAT_MIN_PIN_MV   = 100.0f;  // below: pin grounded / divider open
static const float VBAT_MAX_SPREAD_MV = 150.0f; // across 16 samples; a floating pin wanders

// --------------------------------------------------------------- state ------
enum CamState : uint8_t {
  CAM_OFF,          // rail open
  CAM_BOOTING,      // rail closed, camera still coming up
  CAM_ON,           // ready (standby, or recording when `recording` is set)
  CAM_SAVING,       // stop-record sent, file closing, rail stays on
  CAM_STOPPING,     // stop-record sent, file closing, then rail opens
};

static CamState camState = CAM_OFF;
static uint32_t stateSince = 0;
static bool recording = false;
static uint32_t recSince = 0;       // millis() when REC was sent
// The camera has just been powered and has not yet had its one AUTOREC stop.
// Without a load switch the camera powers up with the ESP32, so this starts
// true at boot; with one, every Standby re-powers the camera.
static bool camFreshPower = true;
// AUTOREC: waiting to stop the camera's auto-started recording. REC is
// refused until then, or it would toggle that recording off.
static bool autoStopPending = false;
static uint32_t onTimeMs = 0;       // accumulated rail-on time
static uint32_t onSince = 0;

static const uint32_t BOOT_MS       = 3000;  // Split 4 needs ~2-3 s to be ready
// Grace period after stop-record. The camera never ACKs that the file is
// closed, so this is a blind wait; MP4 finalisation (writing the index) takes
// longer on long clips and slow cards, so err on the generous side.
static const uint32_t FILE_CLOSE_MS = 5000;

// Camera presses closer together than this can be merged or both counted;
// with the power-button toggle that leaves REC/STOP inverted.
static const uint32_t MIN_PRESS_GAP_MS = 2000;

// AUTOREC ON: how long after the camera gets power before the ESP32 stops its
// auto-started recording. The camera must have booted, mounted the card and
// actually begun recording by then, or the press would START a recording.
// Time yours (power on -> recording light blinking) and keep this longer.
static const uint32_t AUTOREC_SETTLE_MS = 10000;

// ----------------------------------------------------- tunable settings -----
// How REC/STOP talk to the camera. Some Split firmware ignores the dedicated
// start/stop actions; on those, a simulated power-button press toggles
// recording instead. The toggle is blind: if the camera is already recording
// (e.g. its auto-record option is on), REC will STOP it.
enum RecMethod : uint8_t { REC_CMD = 0, REC_PWRBTN = 1 };

// The camera accepts 5-20 V, so it regulates internally and behaves as a
// roughly constant-POWER load: as the pack sags, the current it pulls rises.
// Endurance therefore has to be computed in watt-hours, not mAh -- a fixed-mA
// model would overestimate the tail of the discharge.
struct Settings {
  uint32_t capacityMah;   // pack capacity
  uint32_t camPowerMw;    // camera power while on (MEASURE THIS)
  uint32_t espPowerMw;    // ESP32 + Wi-Fi AP + regulator losses
  uint8_t  cells;         // LiPo cells in series, 0 = auto-detect
  uint8_t  reservePct;    // refuse to plan below this state of charge
  uint8_t  recMethod;     // RecMethod
  uint8_t  autoRec;       // 1 = camera starts recording by itself at power-up
};

// Battery maths result; declared up here so Arduino's auto-prototypes see it.
struct Endurance {
  float volts, cellVolts, soc, packWh, wNow, wFull;
  uint8_t cells;
  bool sensor;          // false: volts/soc/remainingMin are not meaningful
  float remainingMin;   // at present draw, down to the reserve
  float fullMin;        // from 100% with the camera on
};

static Settings cfg = { 450, 2000, 700, 1, 15, REC_CMD, 0 };
static String apSsid = DEFAULT_SSID;
static String apPass = DEFAULT_PASS;
static Preferences prefs;

static RunCam cam;
static WebServer server(80);

// Replies from handleCommand() go to the serial console, and are also
// collected here while an HTTP request is being served so the page can
// show them.
static bool capturing = false;
static String captured;

// BLE state. Writes arrive on the Bluetooth task, so commands are queued and
// run from loop(), never from the callback -- the state machine stays
// single-threaded.
static const size_t BLE_CMD_MAX = 96;
struct BleCmd { char text[BLE_CMD_MAX]; };
static QueueHandle_t bleQueue = nullptr;
static BLEServer *bleServer = nullptr;
static BLECharacteristic *bleTx = nullptr;
static volatile bool bleConnected = false;
static bool bleWasConnected = false;

// ------------------------------------------------------------- helpers ------
static void setRail(bool on) {
  digitalWrite(PIN_CAM_PWR, (on == CAM_PWR_ACTIVE_HIGH) ? HIGH : LOW);
}

static const char *offNote() {
  return LOAD_SWITCH_FITTED
    ? "camera unpowered (no draw)"
    : "NO LOAD SWITCH -- camera still powered; unplug the battery only "
      "once the camera's recording light has stopped blinking";
}

static void enterState(CamState s) {
  camState = s;
  stateSince = millis();
}

// Send a line to the BLE phone app. 20-byte chunks fit the smallest BLE MTU,
// so this works whatever the phone negotiates; terminal apps reassemble them.
static void bleSend(const String &msg) {
  if (!bleConnected || !bleTx) return;
  String line = msg + "\n";
  for (size_t i = 0; i < line.length(); i += 20) {
    size_t n = min((size_t)20, line.length() - i);
    bleTx->setValue((const uint8_t *)line.c_str() + i, n);
    bleTx->notify();
    delay(4);   // let the stack drain; back-to-back notifies can be dropped
  }
}

static void reply(const String &msg) {
  Serial.println(msg);
  bleSend(msg);
  if (capturing) {
    if (captured.length()) captured += '\n';
    captured += msg;
  }
}

static uint8_t cellCount(float v) {
  if (cfg.cells > 0) return cfg.cells;
  int n = (int)lroundf(v / 3.8f);
  return (uint8_t)constrain(n, 1, 6);
}

// Pack volts, or -1 when there is no trustworthy reading (no divider fitted,
// pin grounded, pin floating, or a voltage no LiPo could have).
static float readPackVolts() {
  if (!VBAT_SENSOR_FITTED) return -1.0f;
  uint32_t acc = 0, lo = UINT32_MAX, hi = 0;
  for (int i = 0; i < 16; i++) {
    uint32_t mv = analogReadMilliVolts(PIN_VBAT);
    acc += mv;
    lo = min(lo, mv);
    hi = max(hi, mv);
  }
  float pinMv = acc / 16.0f;
  if (pinMv < VBAT_MIN_PIN_MV || hi - lo > VBAT_MAX_SPREAD_MV) return -1.0f;
  float v = pinMv / 1000.0f * DIV_RATIO;
  float perCell = v / cellCount(v);
  if (perCell < 2.5f || perCell > 4.35f) return -1.0f;
  return v;
}

// Resting-voltage -> state-of-charge for LiPo. Under load this reads low,
// so treat the result as a floor, not a precise gauge.
static float socFromCellVolts(float vc) {
  static const float V[]   = {3.00f,3.30f,3.50f,3.60f,3.70f,3.75f,3.80f,
                              3.85f,3.90f,3.95f,4.00f,4.10f,4.20f};
  static const float SOC[] = {0.00f,0.05f,0.10f,0.20f,0.35f,0.45f,0.55f,
                              0.65f,0.72f,0.80f,0.86f,0.94f,1.00f};
  const int N = sizeof(V) / sizeof(V[0]);
  if (vc <= V[0])     return 0.0f;
  if (vc >= V[N - 1]) return 1.0f;
  for (int i = 0; i < N - 1; i++) {
    if (vc >= V[i] && vc <= V[i + 1]) {
      float t = (vc - V[i]) / (V[i + 1] - V[i]);
      return SOC[i] + t * (SOC[i + 1] - SOC[i]);
    }
  }
  return 0.5f;
}

static uint32_t railOnMs() {
  uint32_t t = onTimeMs;
  if (camState != CAM_OFF) t += millis() - onSince;
  return t;
}

static String fmtMinutes(float minutes) {
  if (minutes < 0) minutes = 0;
  int h = (int)(minutes / 60);
  int m = (int)(minutes - h * 60);
  char buf[32];
  if (h > 0) snprintf(buf, sizeof(buf), "%dh %02dm", h, m);
  else       snprintf(buf, sizeof(buf), "%dm", m);
  return String(buf);
}

static const char *stateName() {
  switch (camState) {
    case CAM_OFF:      return "OFF";
    case CAM_BOOTING:  return "BOOTING";
    case CAM_ON:       return recording ? "RECORDING" : "STANDBY";
    case CAM_SAVING:   return "SAVING";
    case CAM_STOPPING: return "STOPPING";
  }
  return "?";
}

static void saveSettings() {
  prefs.begin("camctl", false);
  prefs.putUInt("cap", cfg.capacityMah);
  prefs.putUInt("camw", cfg.camPowerMw);
  prefs.putUInt("espw", cfg.espPowerMw);
  prefs.putUChar("cells", cfg.cells);
  prefs.putUChar("res", cfg.reservePct);
  prefs.putUChar("recm", cfg.recMethod);
  prefs.putUChar("arec", cfg.autoRec);
  prefs.putString("ssid", apSsid);
  prefs.putString("pass", apPass);
  prefs.end();
}

static void loadSettings() {
  prefs.begin("camctl", true);
  cfg.capacityMah = prefs.getUInt("cap", cfg.capacityMah);
  cfg.camPowerMw  = prefs.getUInt("camw", cfg.camPowerMw);
  cfg.espPowerMw  = prefs.getUInt("espw", cfg.espPowerMw);
  cfg.cells       = prefs.getUChar("cells", cfg.cells);
  cfg.reservePct  = prefs.getUChar("res", cfg.reservePct);
  cfg.recMethod   = prefs.getUChar("recm", cfg.recMethod) == REC_PWRBTN ? REC_PWRBTN : REC_CMD;
  cfg.autoRec     = prefs.getUChar("arec", cfg.autoRec) ? 1 : 0;
  apSsid         = prefs.getString("ssid", apSsid);
  apPass          = prefs.getString("pass", apPass);
  prefs.end();
}

// ------------------------------------------------------------ endurance -----
static Endurance endurance() {
  Endurance e;
  e.volts = readPackVolts();
  e.sensor = e.volts > 0;
  // Without a reading, auto-detect has nothing to go on: assume the configured
  // count, or 1S.
  e.cells = e.sensor ? cellCount(e.volts) : (cfg.cells ? cfg.cells : 1);
  e.cellVolts = e.sensor ? e.volts / e.cells : 0;
  e.soc = e.sensor ? socFromCellVolts(e.cellVolts) : 0;

  float reserve = cfg.reservePct / 100.0f;
  e.packWh = cfg.capacityMah / 1000.0f * e.cells * 3.7f;   // nominal, for Wh
  float availWh = e.packWh * max(0.0f, e.soc - reserve);

  float camW = cfg.camPowerMw / 1000.0f;
  float espW = cfg.espPowerMw / 1000.0f;
  e.wNow  = espW + (camState == CAM_OFF ? 0.0f : camW);
  e.wFull = espW + camW;
  e.remainingMin = (e.sensor && e.wNow > 0.01f) ? availWh / e.wNow * 60.0f : -1;
  e.fullMin = (e.wFull > 0.01f) ? e.packWh * (1.0f - reserve) / e.wFull * 60.0f : 0;
  return e;
}

// ------------------------------------------------------------- reports ------
static void reportRuntime();

static void reportStatus() {
  Endurance e = endurance();
  reply("--- status ---");
  reply("camera    : " + String(stateName()));
  reply("link      : " + String(cam.responded() ? "UART ok" : "no UART reply"));
  if (e.sensor) {
    reply("pack      : " + String(e.volts, 2) + " V  (" + String(e.cells) + "S, " +
          String(e.cellVolts, 2) + " V/cell)");
    reply("charge    : ~" + String((int)(e.soc * 100)) + "%  (resting-curve est.)");
  } else {
    reply("pack      : no battery sensor  (divider not fitted, or reading invalid)");
  }
  reply("rec method: " + String(cfg.recMethod == REC_PWRBTN ? "PWRBTN (power-button toggle)"
                                                             : "CMD (start/stop commands)"));
  reply("autorec   : " + String(cfg.autoRec ? "ON" : "OFF") +
        (autoStopPending ? "  (waiting to stop the camera's own recording)" : ""));
  reply("rail on   : " + fmtMinutes(railOnMs() / 60000.0f));
  reply("bluetooth : " + String(BLE_NAME) + (bleConnected ? "  (phone connected)" : "  (advertising)"));
  if (USE_WIFI)
    reply("wifi      : " + apSsid + "  " + WiFi.softAPIP().toString() +
          "  (" + String(WiFi.softAPgetStationNum()) + " connected)");
  else
    reply("wifi      : off");
  reportRuntime();
}

static void reportRuntime() {
  Endurance e = endurance();
  reply("--- endurance ---");
  reply("pack      : " + String(cfg.capacityMah) + " mAh @ " + String(e.cells) + "S = " +
        String(e.packWh, 1) + " Wh  (reserve " + String(cfg.reservePct) + "%)");
  reply("power     : camera " + String(cfg.camPowerMw / 1000.0f, 2) + " W + esp " +
        String(cfg.espPowerMw / 1000.0f, 2) + " W");
  if (!e.sensor) {
    reply("remaining : unknown -- no battery sensor");
  } else if (e.wNow > 0.01f) {
    reply("remaining : " + fmtMinutes(e.remainingMin) +
          "  at the present " + String(e.wNow, 2) + " W");
  }
  reply("full pack : " + fmtMinutes(e.fullMin) + "  from 100% with camera on");
  if (e.sensor) {
    reply("pack draw : ~" + String((int)(e.wFull / e.volts * 1000.0f)) + " mA at " +
          String(e.volts, 1) + " V  (rises as the pack sags)");
  }
  reply("note      : camera power is an assumption until you measure it -- "
        "WATTS <mW>, or DRAW <mA> to convert from a current reading.");
}

// ------------------------------------------------------------ commands ------
// Each returns true if it did what was asked, so the HTTP API can report
// success or failure accurately.

// Send whichever start/stop the configured RecMethod calls for.
static uint32_t lastPressMs = 0;
static void spacePresses() {
  uint32_t since = millis() - lastPressMs;
  if (lastPressMs && since < MIN_PRESS_GAP_MS) delay(MIN_PRESS_GAP_MS - since);
  lastPressMs = millis();
}

static void camRecord(bool start) {
  spacePresses();
  if (cfg.recMethod == REC_PWRBTN) cam.simulatePowerButton();
  else if (start)                  cam.startRecording();
  else                             cam.stopRecording();
}

static bool doStandby() {
  if (camState == CAM_ON && !recording) { reply("already in standby"); return true; }
  if (camState == CAM_BOOTING)          { reply("camera is starting");  return true; }
  if (camState == CAM_ON && recording)  { reply("recording -- stop it first"); return false; }
  if (camState != CAM_OFF)              { reply("camera is saving a file -- wait"); return false; }
  setRail(true);
  onSince = millis();
  if (LOAD_SWITCH_FITTED) camFreshPower = true;
  enterState(CAM_BOOTING);
  reply("power on; camera starting (~3 s)");
  return true;
}

static bool doRecord() {
  if (camState != CAM_ON) { reply("camera not ready -- press Standby first"); return false; }
  if (recording)          { reply("already recording"); return true; }
  if (autoStopPending) {
    reply("camera still settling after power-up (AUTOREC) -- REC will be ready in a few seconds");
    return false;
  }
  camRecord(true);
  recording = true;
  recSince = millis();
  reply("recording started");
  return true;
}

// Stop and give the camera time to close the file before anything else.
static bool doStopSave() {
  if (camState != CAM_ON || !recording) { reply("not recording"); return false; }
  camRecord(false);
  recording = false;
  enterState(CAM_SAVING);
  reply("recording stopped; saving file...");
  return true;
}

static bool doPowerOff() {
  if (camState == CAM_OFF)      { reply("already off"); return true; }
  if (camState == CAM_STOPPING) { reply("already shutting down"); return true; }

  if (recording) {
    camRecord(false);
    recording = false;
    enterState(CAM_STOPPING);
    reply("stopping recording, closing file before cutting power...");
  } else if (camState == CAM_SAVING) {
    enterState(CAM_STOPPING);   // restart the grace period, then cut
    reply("power will cut once the file is saved");
  } else {
    setRail(false);
    onTimeMs += millis() - onSince;
    enterState(CAM_OFF);
    reply("power off; " + String(offNote()));
  }
  return true;
}

static bool handleCommand(String line) {
  line.trim();
  if (!line.length()) return false;

  // Verb is case-insensitive; the argument keeps its case (SSID / PASS).
  int sp = line.indexOf(' ');
  String verb = (sp < 0) ? line : line.substring(0, sp);
  String arg  = (sp < 0) ? ""   : line.substring(sp + 1);
  verb.toUpperCase();
  arg.trim();

  if (verb == "ON" || verb == "STANDBY") return doStandby();
  if (verb == "REC")                     return doRecord();
  if (verb == "STOP")                    return doStopSave();
  if (verb == "OFF")                     return doPowerOff();

  if (verb == "PWRBTN" || verb == "WIFI" || verb == "MODE") {
    if (camState != CAM_ON) { reply("camera not ready"); return false; }
    if (verb == "PWRBTN") {
      spacePresses();
      cam.simulatePowerButton();
      reply("power-button press sent (toggles recording; the app's REC state is NOT "
            "updated -- use SYNC REC / SYNC IDLE to match the camera light)");
    } else if (verb == "WIFI") {
      cam.simulateWifiButton(); reply("wifi-button press sent");
    } else {
      cam.changeMode(); reply("mode-change sent");
    }
    return true;
  }
  if (verb == "INFO") {
    bool ok = cam.probe();
    reply("--- camera ---");
    reply("replied   : " + String(cam.responded() ? "yes" : "no"));
    reply("parsed    : " + String(ok ? "yes" : "no"));
    reply("protocol  : v" + String(cam.protocolVersion()));
    reply("features  : " + cam.featureList());
    reply("raw       : " + cam.lastRawReply());
    if (!cam.responded())
      reply("hint      : check TX/RX are crossed and the camera is powered");
    return ok;
  }
  if (verb == "FRAMES") {
    // The exact bytes each command puts on the wire, to check the CRC
    // against the protocol spec without a logic analyser.
    reply("--- outgoing frames ---");
    reply("get-device-info : " + cam.frameHex(RunCam::PUB_GET_DEVICE_INFO));
    reply("power-button    : " + cam.frameHex(RunCam::PUB_CAMERA_CONTROL,
                                              RunCam::PUB_ACT_POWER_BTN));
    reply("change-mode     : " + cam.frameHex(RunCam::PUB_CAMERA_CONTROL,
                                              RunCam::PUB_ACT_CHANGE_MODE));
    reply("start-recording : " + cam.frameHex(RunCam::PUB_CAMERA_CONTROL,
                                              RunCam::PUB_ACT_START_REC));
    reply("stop-recording  : " + cam.frameHex(RunCam::PUB_CAMERA_CONTROL,
                                              RunCam::PUB_ACT_STOP_REC));
    return true;
  }
  if (verb == "SYNC") {
    // Correct what the ESP32 believes, without pressing anything. Use it when
    // the camera light and the app disagree.
    String m = arg;
    m.toUpperCase();
    if (camState != CAM_ON) { reply("camera not ready -- press Standby first"); return false; }
    if (m == "REC") {
      if (!recording) { recording = true; recSince = millis(); }
      autoStopPending = false;
      reply("noted: camera IS recording (nothing sent) -- STOP will stop it");
      return true;
    }
    if (m == "IDLE") {
      recording = false;
      autoStopPending = false;
      reply("noted: camera is NOT recording (nothing sent) -- REC will start it");
      return true;
    }
    reply("usage: SYNC REC | SYNC IDLE  (say what the camera light shows)");
    return false;
  }
  if (verb == "AUTOREC") {
    String m = arg;
    m.toUpperCase();
    if (m == "ON" || m == "OFF") {
      cfg.autoRec = (m == "ON") ? 1 : 0;
      saveSettings();
    } else if (m.length()) {
      reply("usage: AUTOREC ON | OFF"); return false;
    }
    reply(cfg.autoRec
          ? "autorec = ON: after power-up the ESP32 stops the camera's own recording, then REC is ready"
          : "autorec = OFF: the camera is assumed idle at power-up");
    return true;
  }
  if (verb == "RECMODE") {
    String m = arg;
    m.toUpperCase();
    if (m == "CMD" || m == "PWRBTN") {
      if (recording) { reply("stop recording before changing the method"); return false; }
      cfg.recMethod = (m == "PWRBTN") ? REC_PWRBTN : REC_CMD;
      saveSettings();
    } else if (m.length()) {
      reply("usage: RECMODE CMD | PWRBTN"); return false;
    }
    reply(cfg.recMethod == REC_PWRBTN
          ? "rec method = PWRBTN (power-button toggle; blind -- watch the camera LED)"
          : "rec method = CMD (start/stop-recording commands)");
    return true;
  }
  if (verb == "STATUS")  { reportStatus();  return true; }
  if (verb == "RUNTIME") { reportRuntime(); return true; }

  // ---- settings (all saved to flash) ----
  if (arg.length()) {
    long n = arg.toInt();
    if ((verb == "CAP" || verb == "WATTS" || verb == "ESPWATTS" || verb == "DRAW") && n <= 0) {
      reply(verb + " needs a positive whole number"); return false;
    }
    if (verb == "CAP") {
      cfg.capacityMah = n; saveSettings();
      reply("capacity = " + String(cfg.capacityMah) + " mAh"); return true;
    }
    if (verb == "WATTS") {
      cfg.camPowerMw = n; saveSettings();
      reply("camera power = " + String(cfg.camPowerMw / 1000.0f, 2) + " W"); return true;
    }
    if (verb == "DRAW") {
      // Convert a pack-side current reading into power at the present voltage.
      float v = readPackVolts();
      if (v < 1.0f) { reply("no valid pack voltage -- use WATTS <mW> instead"); return false; }
      cfg.camPowerMw = (uint32_t)(n * v); saveSettings();
      reply("measured " + arg + " mA at " + String(v, 2) + " V -> camera power = " +
            String(cfg.camPowerMw / 1000.0f, 2) + " W");
      return true;
    }
    if (verb == "ESPWATTS") {
      cfg.espPowerMw = n; saveSettings();
      reply("esp power = " + String(cfg.espPowerMw / 1000.0f, 2) + " W"); return true;
    }
    if (verb == "CELLS") {
      cfg.cells = constrain(n, 0, 6); saveSettings();
      reply("cells = " + String(cfg.cells) + (cfg.cells ? "" : " (auto)")); return true;
    }
    if (verb == "RESERVE") {
      cfg.reservePct = constrain(n, 0, 50); saveSettings();
      reply("reserve = " + String(cfg.reservePct) + "%"); return true;
    }
    if (verb == "SSID") {
      if (arg.length() > 32) { reply("SSID must be 1-32 characters"); return false; }
      apSsid = arg; saveSettings();
      reply("Wi-Fi name = " + apSsid + "  (takes effect after restart)"); return true;
    }
    if (verb == "PASS") {
      if (arg.length() < 8 || arg.length() > 63) {
        reply("password must be 8-63 characters"); return false;
      }
      apPass = arg; saveSettings();
      reply("Wi-Fi password changed  (takes effect after restart)"); return true;
    }
  }
  if (verb == "RESTART") {
    if (camState != CAM_OFF) { reply("power the camera off first"); return false; }
    reply("restarting...");
    delay(200);
    ESP.restart();
  }
  if (verb == "HELP" || verb == "?") {
    reply("STANDBY (ON)    power the camera and get ready");
    reply("REC             start recording");
    reply("STOP            stop recording and save the file");
    reply("OFF             cut camera power (saves any recording first)");
    reply("PWRBTN          simulate power button (camera standby, still powered)");
    reply("WIFI / MODE     simulate wifi button / change mode");
    reply("RECMODE CMD|PWRBTN  how REC/STOP reach the camera (saved)");
    reply("AUTOREC ON|OFF  camera records by itself at power-up? (saved)");
    reply("SYNC REC|IDLE   tell the ESP32 what the camera light shows (sends nothing)");
    reply("INFO            probe camera over UART");
    reply("FRAMES          dump outgoing protocol bytes");
    reply("STATUS          state, pack voltage, on-time");
    reply("RUNTIME         endurance estimate");
    reply("CAP <mAh>       set pack capacity");
    reply("WATTS <mW>      set measured camera power");
    reply("DRAW <mA>       set power from a pack-side current reading");
    reply("ESPWATTS <mW>   set esp32 power");
    reply("CELLS <n>       cells in series (0 = auto)");
    reply("RESERVE <pct>   reserve floor");
    reply("SSID <name>     Wi-Fi network name (after RESTART; Wi-Fi builds only)");
    reply("PASS <pw>       Wi-Fi password, 8-63 chars (after RESTART; Wi-Fi builds only)");
    reply("RESTART         reboot the ESP32 (camera must be off)");
    return true;
  }
  reply("unknown: " + line + "   (HELP for commands)");
  return false;
}

// ------------------------------------------------------------- web API ------
static String jsonEscape(const String &s) {
  String out;
  out.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if (c == '\n')        out += "\\n";
    else if ((uint8_t)c < 0x20) out += ' ';
    else                       out += c;
  }
  return out;
}

static String statusJson() {
  Endurance e = endurance();
  uint32_t recSec = recording ? (millis() - recSince) / 1000 : 0;
  String j = "{";
  j += "\"state\":\"" + String(stateName()) + "\"";
  j += ",\"recording\":" + String(recording ? "true" : "false");
  j += ",\"recSec\":" + String(recSec);
  j += ",\"uart\":" + String(cam.responded() ? "true" : "false");
  j += ",\"features\":\"" + jsonEscape(cam.featureList()) + "\"";
  j += ",\"vbat\":" + String(e.sensor ? "true" : "false");
  j += ",\"volts\":" + (e.sensor ? String(e.volts, 2) : String("null"));
  j += ",\"cells\":" + String(e.cells);
  j += ",\"soc\":" + (e.sensor ? String((int)(e.soc * 100)) : String("null"));
  j += ",\"remainingMin\":" + (e.sensor ? String((int)e.remainingMin) : String("null"));
  j += ",\"fullMin\":" + String((int)e.fullMin);
  j += ",\"onMin\":" + String(railOnMs() / 60000);
  j += ",\"settings\":{\"capacityMah\":" + String(cfg.capacityMah) +
       ",\"camPowerMw\":" + String(cfg.camPowerMw) +
       ",\"espPowerMw\":" + String(cfg.espPowerMw) +
       ",\"cells\":" + String(cfg.cells) +
       ",\"reservePct\":" + String(cfg.reservePct) +
       ",\"recMethod\":\"" + String(cfg.recMethod == REC_PWRBTN ? "PWRBTN" : "CMD") + "\"" +
       ",\"ssid\":\"" + jsonEscape(apSsid) + "\"}";
  j += "}";
  return j;
}

static void sendJson(int code, const String &body) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", body);
}

// Run a command and answer with {ok, msg, status}.
static void apiRun(const String &cmd) {
  capturing = true;
  captured = "";
  bool ok = handleCommand(cmd);
  capturing = false;
  sendJson(ok ? 200 : 409,
           "{\"ok\":" + String(ok ? "true" : "false") +
           ",\"msg\":\"" + jsonEscape(captured) + "\"" +
           ",\"status\":" + statusJson() + "}");
}

static void setupWeb() {
  server.on("/", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/html", WEB_UI);
  });

  server.on("/api/status", HTTP_GET, []() { sendJson(200, statusJson()); });

  // Control actions are POST-only so a browser prefetch can never trigger one.
  server.on("/api/standby", HTTP_POST, []() { apiRun("STANDBY"); });
  server.on("/api/record",  HTTP_POST, []() { apiRun("REC"); });
  server.on("/api/stop",    HTTP_POST, []() { apiRun("STOP"); });
  server.on("/api/off",     HTTP_POST, []() { apiRun("OFF"); });

  // Any console command, e.g. POST /api/cmd with c=INFO
  server.on("/api/cmd", HTTP_POST, []() {
    if (!server.hasArg("c")) { sendJson(400, "{\"ok\":false,\"msg\":\"missing c\"}"); return; }
    apiRun(server.arg("c"));
  });

  server.onNotFound([]() {
    // Any other path (a mistyped URL, favicon) lands on the page.
    server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/");
    server.send(302, "text/plain", "");
  });

  server.begin();
}

// ----------------------------------------------------------- BLE link -------
class BleServerCb : public BLEServerCallbacks {
  void onConnect(BLEServer *) override    { bleConnected = true; }
  void onDisconnect(BLEServer *) override { bleConnected = false; }
};

class BleRxCb : public BLECharacteristicCallbacks {
  // Runs on the Bluetooth task: only split and queue, never act here.
  // A write may hold one command with no line ending, or several lines.
  void onWrite(BLECharacteristic *c) override {
    String v = c->getValue();
    int start = 0;
    while (start <= (int)v.length()) {
      int end = start;
      while (end < (int)v.length() && v[end] != '\n' && v[end] != '\r') end++;
      String part = v.substring(start, end);
      part.trim();
      if (part.length()) {
        BleCmd cmd;
        strlcpy(cmd.text, part.c_str(), sizeof(cmd.text));
        xQueueSend(bleQueue, &cmd, 0);   // full queue: drop rather than block
      }
      start = end + 1;
    }
  }
};

static void setupBle() {
  bleQueue = xQueueCreate(8, sizeof(BleCmd));
  BLEDevice::init(BLE_NAME);
  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new BleServerCb());

  BLEService *svc = bleServer->createService(NUS_SERVICE);
  bleTx = svc->createCharacteristic(NUS_TX, BLECharacteristic::PROPERTY_NOTIFY);
  bleTx->addDescriptor(new BLE2902());   // Bluedroid needs the CCCD added by hand
  BLECharacteristic *rx = svc->createCharacteristic(
      NUS_RX, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  rx->setCallbacks(new BleRxCb());
  svc->start();

  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(NUS_SERVICE);   // lets UART apps filter for us
  adv->setScanResponse(true);         // name moves to the scan response
  adv->setMinPreferred(0x06);         // connection-interval hints iPhones prefer
  adv->setMaxPreferred(0x12);
  BLEDevice::startAdvertising();
}

// Run queued phone commands and keep advertising whenever nobody is connected.
static void serviceBle() {
  BleCmd cmd;
  while (xQueueReceive(bleQueue, &cmd, 0) == pdTRUE) {
    Serial.println(String("[ble] > ") + cmd.text);
    handleCommand(String(cmd.text));
  }
  if (bleConnected && !bleWasConnected) {
    bleWasConnected = true;
    Serial.println("[ble] phone connected");
    delay(300);   // give the app a moment to subscribe before we talk
    reply("RunCam-Ctl connected. Camera: " + String(stateName()) + "  (HELP for commands)");
  } else if (!bleConnected && bleWasConnected) {
    bleWasConnected = false;
    Serial.println("[ble] phone disconnected; advertising again");
    delay(200);
    bleServer->startAdvertising();
  }
}

// ------------------------------------------------------------- lifecycle ----
void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_CAM_PWR, OUTPUT);
  setRail(false);               // start with the camera unpowered, never surprise-drain
  enterState(CAM_OFF);

  analogSetPinAttenuation(PIN_VBAT, ADC_11db);

  loadSettings();
  cam.begin(Serial2, PIN_CAM_RX, PIN_CAM_TX);

  Serial.println();
  Serial.println("=== RunCam controller ===");
  Serial.printf("chip %s rev %d, %lu MHz\n", ESP.getChipModel(),
                ESP.getChipRevision(), (unsigned long)getCpuFrequencyMhz());

  setupBle();
  Serial.printf("[ble] advertising as \"%s\" (Nordic UART) -- connect with a BLE UART app\n",
                BLE_NAME);

  if (!USE_WIFI) {
    WiFi.mode(WIFI_OFF);
    Serial.println("[wifi] off (USE_WIFI = false)");
    Serial.println("Type HELP here or in the BLE app for commands.");
    return;
  }

  WiFi.mode(WIFI_AP);
  if (!WiFi.softAP(apSsid.c_str(), apPass.c_str(), WIFI_CHANNEL)) {
    // A bad saved password must not lock you out: fall back to the defaults.
    Serial.println("[wifi] saved settings rejected; using defaults");
    apSsid = DEFAULT_SSID;
    apPass = DEFAULT_PASS;
    WiFi.softAP(apSsid.c_str(), apPass.c_str(), WIFI_CHANNEL);
  }
  if (MDNS.begin(MDNS_NAME)) MDNS.addService("http", "tcp", 80);
  setupWeb();

  Serial.printf("[wifi] network \"%s\"  password \"%s\"\n", apSsid.c_str(), apPass.c_str());
  Serial.printf("[wifi] open http://%s  or  http://%s.local\n",
                WiFi.softAPIP().toString().c_str(), MDNS_NAME);
  Serial.println("Type HELP here for console commands.");
}

void loop() {
  if (USE_WIFI) server.handleClient();
  serviceBle();

  // --- state machine ---------------------------------------------------
  uint32_t now = millis();

  if (camState == CAM_BOOTING && now - stateSince >= BOOT_MS) {
    enterState(CAM_ON);
    bool ok = cam.probe();
    reply(ok ? "camera ready (UART ok, features: " + cam.featureList() + ")"
             : "camera powered, but no valid UART reply -- try INFO");
    if (ok && cfg.recMethod == REC_CMD && !cam.supports(RunCam::FEAT_START_RECORDING))
      reply("note: camera does not report start/stop-rec support -- "
            "if REC does nothing, try RECMODE PWRBTN");
    // Only the first Standby after the camera gets power: a camera that was
    // already stopped once this power cycle is idle, and a press would start it.
    autoStopPending = cfg.autoRec && camFreshPower;
    camFreshPower = false;
    if (autoStopPending) reply("AUTOREC: will stop the camera's own recording once it has settled...");
  }

  // AUTOREC: stop the recording the camera started by itself at power-up, so
  // REC/STOP begin from a known idle camera. Without a load switch the camera
  // powered up with the ESP32, so its uptime is the ESP32's.
  if (autoStopPending && camState == CAM_ON && !recording) {
    uint32_t camUp = LOAD_SWITCH_FITTED ? now - onSince : now;
    if (camUp >= AUTOREC_SETTLE_MS) {
      autoStopPending = false;
      camRecord(false);
      reply("AUTOREC: stopped the camera's own recording -- light should show standby; REC is ready");
    }
  }

  if (camState == CAM_SAVING && now - stateSince >= FILE_CLOSE_MS) {
    enterState(CAM_ON);
    reply("file saved; camera in standby");
  }

  if (camState == CAM_STOPPING && now - stateSince >= FILE_CLOSE_MS) {
    setRail(false);
    onTimeMs += now - onSince;
    enterState(CAM_OFF);
    reply("file saved; power off; " + String(offNote()));
  }

  // --- USB serial console (handy for bench work) -------------------------
  static String serialLine;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialLine.length()) { handleCommand(serialLine); serialLine = ""; }
    } else if (serialLine.length() < 80) {
      serialLine += c;
    }
  }

  // --- status LED ------------------------------------------------------
  // off: slow pulse   booting/saving: fast blink   standby: solid
  // recording: double flash
  static uint32_t ledAt = 0;
  static bool ledOn = false;
  if (camState == CAM_ON && recording) {
    uint32_t ph = now % 1000;
    digitalWrite(PIN_LED, (ph < 80 || (ph > 200 && ph < 280)) ? HIGH : LOW);
  } else {
    uint32_t period = (camState == CAM_OFF) ? 1200
                    : (camState == CAM_ON)  ? 0 : 150;
    if (period == 0) {
      digitalWrite(PIN_LED, HIGH);
    } else if (now - ledAt >= period) {
      ledAt = now;
      ledOn = !ledOn;
      digitalWrite(PIN_LED, ledOn ? HIGH : LOW);
    }
  }

  delay(2);
}
