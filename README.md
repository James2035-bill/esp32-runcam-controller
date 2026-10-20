# ESP32 RunCam Split 4 Bluetooth controller

Start and stop a **RunCam Split 4** from your phone over Bluetooth. Built for a
rocket payload camera, but it works for anything that carries a Split 4.

- An **ESP32** talks to the camera over its UART control port (the RunCam
  Device Protocol, the same one Betaflight and iNav use).
- Your **phone** talks to the ESP32 over Bluetooth LE, using a one-page web
  remote: **Wake camera → Start recording → Stop & save**.
- Video is recorded by the camera, straight to **its own microSD card**. The
  ESP32 never touches video.

**Phone remote:** https://james2035-bill.github.io/esp32-runcam-controller/
(on iPhone, open it in the free **Bluefy** browser; see [Phone remote](#5-open-the-phone-remote)).

```
  Phone (Bluefy / Chrome)          ESP32 DevKit                RunCam Split 4
  ┌───────────────────┐  BLE   ┌────────────────┐  UART  ┌────────────────────┐
  │  RunCam Remote    │ ◄────► │  RunCam-Ctl    │ ─────► │ records to its own │
  │  web page         │        │  (this sketch) │ ◄───── │ microSD card       │
  └───────────────────┘        └────────────────┘        └────────────────────┘
```

---

## 1. Parts

This is the build it was developed and tested on. Similar parts work; the
notes say what matters.

| Part | Notes |
|---|---|
| **ESP32 DevKit V1** (ESP-WROOM-32) | Any ESP32 *WROOM* DevKit. A WROVER module uses GPIO16/17 for PSRAM, so the UART would have to move. |
| **RunCam Split 4** + microSD card | Use a good card (U3), formatted **in the camera**. |
| **5 V supply, at least 1.5 A** | Tested: **1S 450 mAh LiPo → 5 V boost converter**. On the bench, a USB wall charger or power bank (≥ 1 A) works too. |
| **1S LiPo charger** | The ESP32 does not charge the battery. Charge at ≤ 1C (450 mA for a 450 mAh pack). |
| Jumper wires, multimeter | The multimeter is for the power check in step 2. |
| **Phone** | iPhone with the free **Bluefy** browser, or Android with Chrome. A Mac with Chrome also works. |

**Not suitable as the camera's power source:** a computer's USB port, or the
5 V pin of a dev board like an STM32 Nucleo. They can't supply the camera's
current, and the camera will brown out and restart, dimming and flashing its
lights and leaving broken files.

## 2. Wiring

| ESP32 pin | Connects to | |
|---|---|---|
| **GPIO16 (RX2)** | camera **TX** | crossed: TX goes to RX |
| **GPIO17 (TX2)** | camera **RX** | crossed: RX goes to TX |
| **GND** | camera **GND** and supply **GND** | one shared ground; the UART needs it |
| **VIN** | supply **5 V** | the same 5 V also feeds the camera's power input |
| GPIO13 | *(optional)* load-switch enable | see [HARDWARE.md](HARDWARE.md) |
| GPIO34 | *(optional)* battery-voltage divider | see [HARDWARE.md](HARDWARE.md) |

```
   5 V supply (boost converter / USB charger)
     +5V ─────────┬──────────────────────────► camera VCC (+)
                  └──────────────────────────► ESP32 VIN
     GND ─────────┬──────────────────────────► camera GND
                  └──────────────────────────► ESP32 GND

   ESP32 GPIO16 (RX2) ◄────────────────────── camera TX
   ESP32 GPIO17 (TX2) ───────────────────────► camera RX
```

- Leave the ESP32's **RX0/TX0** pins alone: they are its USB link to your
  computer.
- The camera's **yellow video wire** is not needed. Tape its end so it can't
  short. (Connect it to an FPV monitor or a USB video capture adapter if you
  want a live view or the camera's settings menu.)
- **Check before powering:** no bare wire ends touching, and + / − the right
  way round. With everything connected, measure the 5 V rail at the camera:
  it should read **4.75–5.5 V**, including while recording.
- Use only **one** source on the 5 V rail at a time. When the ESP32 is plugged
  into your computer by USB, power the camera separately (shared GND).

More detail (logic levels, load switch, battery divider, runtime tables) is in
[HARDWARE.md](HARDWARE.md).

## 3. Install the tools and flash the ESP32

You need **arduino-cli** and Espressif's **esp32 board package 3.3.12**
(tested). On macOS:

```sh
brew install arduino-cli
arduino-cli config init
arduino-cli config add board_manager.additional_urls \
  https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32@3.3.12
```

Then get this code and flash it, with the ESP32 plugged in by a **data** USB
cable (charge-only cables don't create a serial port):

```sh
git clone https://github.com/James2035-bill/esp32-runcam-controller.git
cd esp32-runcam-controller
make flash
```

`make flash` compiles (silently, for **1–5 minutes** the first time: don't
press Ctrl-C), uploads, then opens the serial monitor. Expect a burst of `??`
garbage first (the bootloader at another baud rate), then:

```
=== RunCam controller ===
[ble] advertising as "RunCam-Ctl" (Nordic UART) -- connect with a BLE UART app
[wifi] off (USE_WIFI = false)
```

Press **Ctrl-C** to leave the monitor.

**The Bluetooth build is about 1.7 MB, so it needs the "Huge APP" partition
scheme.** The Makefile already sets it. Using the **Arduino IDE** instead:
board **ESP32 Dev Module**, *Tools → Partition Scheme → Huge APP*, then Upload.

**Upload stuck at `Connecting....`?** Hold the **BOOT** button on the ESP32
until the percentages start.

## 4. One-time setup

These settings are saved in the ESP32's flash. Send them from the serial
monitor (`make monitor`) or from the phone remote's **Advanced** console.

**1. Recording method.** The Split 4 ignores the protocol's dedicated
start/stop commands; a simulated power-button press is what toggles recording:

```
RECMODE PWRBTN
```

**2. Does your camera record by itself at power-up?** Put the card in, power
the camera on, send nothing, and watch its blue light for 15 seconds:

| Light settles to | Your camera… | Send |
|---|---|---|
| **slow blink** | starts recording at power-up (the Split 4's default) | `AUTOREC ON` |
| **solid** | powers up idle | `AUTOREC OFF` |
| **fast blink** | isn't reading the card: reseat or reformat it first | |

With `AUTOREC ON`, tapping **Wake camera** stops that power-up recording
(about 10 s after power-on), so Start and Stop then do what they say.

**3. Your battery** (for the runtime estimate):

```
CAP 450
CELLS 1
```

Check with `STATUS`: it should show `rec method: PWRBTN`, your `autorec`
setting, and `boot : normal`.

## 5. Open the phone remote

Web Bluetooth only works from an `https://` address (or `localhost`) and
only in some browsers:

| Device | How |
|---|---|
| **iPhone / iPad** | Install **Bluefy** (free, App Store). Open the remote link in Bluefy. Safari can't use Bluetooth. |
| **Android** | Open the remote link in **Chrome**. |
| **Mac / Windows** | Chrome, with the link above. Or run `make remote` and open http://localhost:8000. That only works on the same computer, not on your phone. |

Remote link: **https://james2035-bill.github.io/esp32-runcam-controller/**
(served from [`docs/index.html`](docs/index.html) by GitHub Pages; if you fork
this repo, turn on Pages for the `main` branch, `/docs` folder, and use your
own link).

**RunCam-Ctl won't appear in your phone's Bluetooth settings.** That's
normal for this kind of device: tap **Connect** in the remote and pick it from
the list there.

## 6. Record

1. Power everything on (card in the camera).
2. Open the remote, tap **Connect**, and pick **RunCam-Ctl**.
3. Tap **Wake camera**. Wait for **Ready**: the camera light goes **solid**.
4. Tap **Start recording**: the light **blinks slowly**.
5. Tap **Stop & save**: the light goes **solid**. Wait for **"Video saved"**.
6. Only now cut power. Then take out the card and copy the video.

The remote always shows what the camera's light should be doing. If it
doesn't match, open **Camera light doesn't match?** and tap what you see. That
puts the remote back in step without pressing anything on the camera.

| Camera blue light | Meaning |
|---|---|
| solid | idle, not recording |
| slow blink | recording |
| fast blink | no SD card, or a card problem |

**First time: do the 20-second test.** Record exactly 20 s, stop and save,
power off, and play the newest file on a computer. You should find one
~20-second clip (plus a short clip from power-up if `AUTOREC` is on).

## Keeping recordings safe

An MP4 that loses power before the camera finishes writing it is usually
unplayable, so almost every rule here is about power:

- **Only cut power when the light is solid**, at least 5 s after Stop (the
  remote says "Video saved"). Never pull the SD card with the camera powered.
- **Battery:** with no voltage sensor fitted, the remote estimates what's
  left from the time since power-up, assuming a full pack, and warns at
  ~5 min. It's not an automatic stop: on an estimate alone that could end a
  flight recording early. Charge before every flight.
- **Wake the camera soon after power-up.** With auto-record, the camera
  records from power-on until Wake stops it, filling the card.
- **Auto-record is also a flight safety net:** if power glitches mid-flight,
  the camera restarts and records again by itself. That's why the ESP32 never
  stops the power-up recording until you tap Wake.
- **If the ESP32 restarts on its own** (crash or watchdog), the camera kept
  its power and may be recording, so the remote asks what the light shows and
  blocks Start until you answer.
- **Card:** format it in the camera, keep **loop recording off** (so a full
  card can't overwrite a flight), and tape it in for flight. Secure the
  battery connector: a popped plug mid-flight is the likeliest way to lose
  the video.
- **On the launch pad:** confirm the light is **slow blinking** before you
  walk away. Bluetooth range is roughly 10–30 m, and the camera keeps
  recording without the phone.

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| `camera powered, but no valid UART reply` / "camera isn't answering" | TX and RX must be **crossed** (camera TX → GPIO16, camera RX → GPIO17), GND shared, camera powered. |
| Footage starts **after** you press Stop; Start seems to stop it | The camera records at power-up and the ESP32 doesn't know it. `AUTOREC ON` (step 4). If it happens once, use **Camera light doesn't match?** |
| Camera light **blinks fast** | No SD card, or the card is unreadable. Power off, reseat it, or reformat in the camera. |
| Lights go **dim and flicker**; the ESP32 disappears from USB mid-upload | The supply can't deliver the current, or there's a short. Find shorts first, then use a stronger 5 V source (step 1). |
| `make flash` seems to hang | It's compiling (1–5 min, silent). Don't press Ctrl-C. If it stops at `Connecting....`, hold **BOOT**. |
| `??????` at the start of the serial monitor | Normal: bootloader output at a different baud rate. |
| Remote stuck on "Connecting…" / "No status from the controller" | Flash the latest firmware; older builds don't answer the remote's status request. |
| Remote shows "Check the camera's blue light" | The ESP32 restarted without the camera losing power. Answer what the light is doing. |
| `localhost:8000` doesn't load on the phone | `localhost` means "this device". Use the https link on the phone. |
| RunCam-Ctl isn't in the phone's Bluetooth settings | Normal. Connect from inside the remote (Bluefy / Chrome). |
| No serial port appears | Use a **data** USB cable. `make ports` lists what the computer sees. |

## Console commands

Work on the USB serial console (115200) and in the remote's **Advanced**
console. Case-insensitive. Settings persist in flash.

| Command | Does |
|---|---|
| `STANDBY` (or `ON`) | wake the camera and get ready |
| `REC` / `STOP` | start / stop-and-save recording |
| `OFF` | cut camera power via the load switch, saving first (without a load switch it only tells you when it's safe to unplug) |
| `RECMODE CMD\|PWRBTN` | how REC/STOP reach the camera: dedicated commands, or the power-button toggle (Split 4: `PWRBTN`) |
| `AUTOREC ON\|OFF` | the camera records by itself at power-up; Wake stops it |
| `SYNC REC\|IDLE` | tell the ESP32 what the camera light shows (sends nothing to the camera) |
| `PWRBTN` | press the camera's button once (toggles recording) |
| `WIFI` / `MODE` | simulate the camera's Wi-Fi button / change mode |
| `INFO` | probe the camera over UART and list the features it reports |
| `FRAMES` | print the exact protocol bytes sent (to check against a logic analyser) |
| `STATUS` / `RUNTIME` | state, boot type, settings / battery estimate |
| `CAP <mAh>` / `CELLS <n>` / `RESERVE <pct>` | battery capacity / cells in series (0 = auto) / reserve floor |
| `WATTS <mW>` / `DRAW <mA>` | measured camera power, or derive it from a current reading (needs the divider) |
| `ESPWATTS <mW>` | ESP32 power, for the estimate |
| `SSID <name>` / `PASS <pw>` / `RESTART` | Wi-Fi settings (Wi-Fi builds only); reboot |

`APP` is used by the remote: the firmware answers with one `@S {json}` status
line over Bluetooth.

## Options in the sketch

At the top of [esp32_project.ino](esp32_project.ino):

| Setting | Default | |
|---|---|---|
| `USE_WIFI` | `false` | `true` brings back the older Wi-Fi access point and control page (http://192.168.4.1, network `RunCam-Ctl`, password `runcam1234`: change it with `PASS`). Bluetooth stays on. |
| `LOAD_SWITCH_FITTED` | `false` | set `true` once a load switch on GPIO13 can cut the camera's power |
| `VBAT_SENSOR_FITTED` | `false` | set `true` once a battery divider is on GPIO34 (then the remote shows measured charge) |
| `FILE_CLOSE_MS` | 5000 | wait after Stop before the file counts as saved |
| `AUTOREC_SETTLE_MS` | 10000 | how long after power-up before Wake may stop the camera's own recording; raise it if your camera takes longer to start recording |

## Repository layout

| Path | |
|---|---|
| `esp32_project.ino` | the firmware: state machine, Bluetooth link, console |
| `runcam.h` / `runcam.cpp` | RunCam Device Protocol (CRC8 DVB-S2 frames over UART) |
| `docs/index.html` | the phone remote (Web Bluetooth), served by GitHub Pages |
| `web_ui.h` | the older Wi-Fi control page (only used when `USE_WIFI = true`) |
| `HARDWARE.md` | wiring detail, power, load switch, battery divider, runtimes |
| `Makefile` | `make build`, `make flash`, `make monitor`, `make remote`, `make ports` |
| `tools/gen_intellisense.py`, `.vscode/` | VS Code IntelliSense setup (`make intellisense`) |

## Board notes

- **Status LED (GPIO2):** slow pulse = asleep, fast blink = starting or
  saving, solid = ready, double flash = recording.
- **GPIO6–11** are wired to the internal flash: never use them.
- **GPIO34–39** are input-only. The battery sense is on GPIO34 (ADC1), which
  keeps working with the radio on.
- `Serial` is the USB console at 115200. `Serial2` (GPIO16/17) talks to the
  camera and must not be used for anything else.
- After upgrading the esp32 core or adding a library, run
  `make intellisense` to regenerate VS Code's include paths.
