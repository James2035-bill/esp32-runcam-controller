#include "runcam.h"

void RunCam::begin(HardwareSerial &port, int rxPin, int txPin, uint32_t baud) {
  _port = &port;
  _port->begin(baud, SERIAL_8N1, rxPin, txPin);
}

uint8_t RunCam::crc8Update(uint8_t crc, uint8_t byte) {
  crc ^= byte;
  for (uint8_t i = 0; i < 8; i++) {
    crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0xD5) : (uint8_t)(crc << 1);
  }
  return crc;
}

uint8_t RunCam::crc8(const uint8_t *data, size_t len) {
  uint8_t crc = 0;
  for (size_t i = 0; i < len; i++) crc = crc8Update(crc, data[i]);
  return crc;
}

void RunCam::send(uint8_t cmd, const uint8_t *params, size_t nParams) {
  if (!_port) return;
  uint8_t buf[8];
  size_t n = 0;
  buf[n++] = HEADER;
  buf[n++] = cmd;
  for (size_t i = 0; i < nParams && n < sizeof(buf) - 1; i++) buf[n++] = params[i];
  buf[n] = crc8(buf, n);
  n++;
  _port->write(buf, n);
  _port->flush();
}

size_t RunCam::readBytes(uint8_t *buf, size_t maxLen, uint32_t timeoutMs) {
  if (!_port) return 0;
  size_t n = 0;
  uint32_t start = millis();
  // Keep reading until the line has been quiet for 40 ms or we time out,
  // so we capture the whole reply rather than the first fragment.
  uint32_t lastByte = start;
  while (millis() - start < timeoutMs) {
    while (_port->available() && n < maxLen) {
      buf[n++] = (uint8_t)_port->read();
      lastByte = millis();
    }
    if (n > 0 && millis() - lastByte > 40) break;
    delay(2);
  }
  return n;
}

String RunCam::toHex(const uint8_t *buf, size_t len) {
  String s;
  char tmp[4];
  for (size_t i = 0; i < len; i++) {
    snprintf(tmp, sizeof(tmp), "%02X", buf[i]);
    s += tmp;
    if (i + 1 < len) s += ' ';
  }
  return s;
}

bool RunCam::probe(uint32_t timeoutMs) {
  if (!_port) return false;

  // Drain anything stale before asking.
  while (_port->available()) _port->read();

  _responded = false;
  _protocolVersion = 0;
  _features = 0;

  // GET_DEVICE_INFO takes no parameters: the frame is exactly CC 00 60.
  // Appending a byte (CC 00 00 39) makes the camera ignore the request.
  send(CMD_GET_DEVICE_INFO, nullptr, 0);

  uint8_t rx[32];
  size_t n = readBytes(rx, sizeof(rx), timeoutMs);
  _lastRaw = (n == 0) ? String("(no reply)") : toHex(rx, n);
  if (n == 0) return false;

  _responded = true;

  // Expected reply: CC <version> <features_lo> <features_hi> <crc>
  // Scan for the header in case of line noise, and verify the CRC.
  for (size_t i = 0; i + 5 <= n; i++) {
    if (rx[i] != HEADER) continue;
    if (crc8(&rx[i], 4) == rx[i + 4]) {
      _protocolVersion = rx[i + 1];
      _features = (uint16_t)rx[i + 2] | ((uint16_t)rx[i + 3] << 8);
      return true;
    }
  }

  // Some firmware omits the header byte on replies. Try that shape too.
  if (n >= 4 && crc8(rx, 3) == rx[3]) {
    _protocolVersion = rx[0];
    _features = (uint16_t)rx[1] | ((uint16_t)rx[2] << 8);
    return true;
  }

  // Responded, but we could not validate it. _lastRaw has the bytes.
  return false;
}

String RunCam::featureList() const {
  if (_features == 0) return String("(none reported)");
  String s;
  struct { Feature bit; const char *name; } kNames[] = {
    { FEAT_SIMULATE_POWER_BUTTON, "power-button" },
    { FEAT_SIMULATE_WIFI_BUTTON,  "wifi-button" },
    { FEAT_CHANGE_MODE,           "change-mode" },
    { FEAT_SIMULATE_5KEY_OSD,     "5key-osd" },
    { FEAT_DEVICE_SETTINGS,       "settings" },
    { FEAT_DISPLAY_PORT,          "display-port" },
    { FEAT_START_RECORDING,       "start-rec" },
    { FEAT_STOP_RECORDING,        "stop-rec" },
    { FEAT_CMS_MENU,              "cms-menu" },
  };
  for (auto &e : kNames) {
    if (supports(e.bit)) {
      if (s.length()) s += ", ";
      s += e.name;
    }
  }
  return s.length() ? s : String("(none recognised)");
}

bool RunCam::cameraControl(uint8_t action) {
  if (!_port) return false;
  const uint8_t params[1] = { action };
  send(CMD_CAMERA_CONTROL, params, 1);
  return true;   // CAMERA_CONTROL is fire-and-forget; the camera does not ACK.
}

bool RunCam::simulatePowerButton() { return cameraControl(ACT_SIMULATE_POWER_BTN); }
bool RunCam::simulateWifiButton()  { return cameraControl(ACT_SIMULATE_WIFI_BTN); }
bool RunCam::changeMode()          { return cameraControl(ACT_CHANGE_MODE); }
bool RunCam::startRecording()      { return cameraControl(ACT_START_RECORDING); }
bool RunCam::stopRecording()       { return cameraControl(ACT_STOP_RECORDING); }

String RunCam::frameHex(uint8_t cmd) {
  uint8_t buf[3];
  buf[0] = HEADER;
  buf[1] = cmd;
  buf[2] = crc8(buf, 2);
  return toHex(buf, 3);
}

String RunCam::frameHex(uint8_t cmd, uint8_t param) {
  uint8_t buf[4];
  buf[0] = HEADER;
  buf[1] = cmd;
  buf[2] = param;
  buf[3] = crc8(buf, 3);
  return toHex(buf, 4);
}
