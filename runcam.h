#pragma once
#include <Arduino.h>

/*
 * RunCam Device Protocol (RCDEVICE) -- the UART control protocol used by
 * RunCam Split / Split 4 and spoken by Betaflight and iNav.
 *
 * Frame format:  0xCC <command> [params...] <crc8>
 * CRC8 is DVB-S2 (polynomial 0xD5, init 0x00), computed across the header
 * byte, the command byte and any parameters.
 *
 * Default link is 115200 8N1.
 *
 * Nothing here is camera-specific guesswork: call probe() and the camera
 * itself reports which of the features below it actually implements.
 */
class RunCam {
public:
  // Feature bits returned by GET_DEVICE_INFO.
  enum Feature : uint16_t {
    FEAT_SIMULATE_POWER_BUTTON = 1 << 0,
    FEAT_SIMULATE_WIFI_BUTTON  = 1 << 1,
    FEAT_CHANGE_MODE           = 1 << 2,
    FEAT_SIMULATE_5KEY_OSD     = 1 << 3,
    FEAT_DEVICE_SETTINGS       = 1 << 4,
    FEAT_DISPLAY_PORT          = 1 << 5,
    FEAT_START_RECORDING       = 1 << 6,
    FEAT_STOP_RECORDING        = 1 << 7,
    FEAT_CMS_MENU              = 1 << 8,
  };

  void begin(HardwareSerial &port, int rxPin, int txPin, uint32_t baud = 115200);

  // Ask the camera who it is. Populates protocolVersion() and features().
  bool probe(uint32_t timeoutMs = 600);

  bool     responded()       const { return _responded; }
  uint8_t  protocolVersion() const { return _protocolVersion; }
  uint16_t features()        const { return _features; }
  bool     supports(Feature f) const { return (_features & (uint16_t)f) != 0; }
  String   featureList()     const;

  // Raw hex of the last bytes the camera sent, for when something looks wrong.
  String   lastRawReply()    const { return _lastRaw; }

  // Build (without sending) the exact bytes a command would put on the wire.
  // Useful for checking the CRC against the protocol docs or a logic analyser.
  String frameHex(uint8_t cmd);                  // no-parameter frame
  String frameHex(uint8_t cmd, uint8_t param);

  bool simulatePowerButton();
  bool simulateWifiButton();
  bool changeMode();
  bool startRecording();
  bool stopRecording();

private:
  // Command IDs
  static const uint8_t CMD_GET_DEVICE_INFO = 0x00;
  static const uint8_t CMD_CAMERA_CONTROL  = 0x01;

  // CAMERA_CONTROL actions
  static const uint8_t ACT_SIMULATE_WIFI_BTN  = 0x00;
  static const uint8_t ACT_SIMULATE_POWER_BTN = 0x01;
  static const uint8_t ACT_CHANGE_MODE        = 0x02;
  static const uint8_t ACT_START_RECORDING    = 0x03;
  static const uint8_t ACT_STOP_RECORDING     = 0x04;

  static const uint8_t HEADER = 0xCC;

public:
  // Exposed so a self-test can enumerate the real command/action IDs.
  static const uint8_t PUB_GET_DEVICE_INFO = CMD_GET_DEVICE_INFO;
  static const uint8_t PUB_CAMERA_CONTROL  = CMD_CAMERA_CONTROL;
  static const uint8_t PUB_ACT_POWER_BTN   = ACT_SIMULATE_POWER_BTN;
  static const uint8_t PUB_ACT_CHANGE_MODE = ACT_CHANGE_MODE;
  static const uint8_t PUB_ACT_START_REC   = ACT_START_RECORDING;
  static const uint8_t PUB_ACT_STOP_REC    = ACT_STOP_RECORDING;
private:

  static uint8_t crc8Update(uint8_t crc, uint8_t byte);
  static uint8_t crc8(const uint8_t *data, size_t len);

  void   send(uint8_t cmd, const uint8_t *params, size_t nParams);
  bool   cameraControl(uint8_t action);
  size_t readBytes(uint8_t *buf, size_t maxLen, uint32_t timeoutMs);
  static String toHex(const uint8_t *buf, size_t len);

  HardwareSerial *_port = nullptr;
  bool     _responded = false;
  uint8_t  _protocolVersion = 0;
  uint16_t _features = 0;
  String   _lastRaw;
};
