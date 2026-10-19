# RunCam Split 4 controller — ESP32 over Bluetooth

Control a RunCam Split 4 from a phone over Bluetooth LE: the ESP32 advertises
as **RunCam-Ctl** and the phone remote in [remote/](remote/index.html) gives
one-tap Wake / Start recording / Stop & save. The older Wi-Fi control page is
still in the firmware behind `USE_WIFI` (off by default, to save battery).
See [HARDWARE.md](HARDWARE.md) for wiring and the runtime numbers.

Board: **ESP32 DevKit V1 (Type-C)** — FQBN `esp32:esp32:esp32` ("ESP32 Dev Module")
Silicon (confirmed on this board): ESP32-D0WD-V3 rev 301, 240 MHz, 4 MB flash
Serial port: shows up as `/dev/cu.usbserial-0001` (macOS native driver, nothing to install)
Toolchain: `arduino-cli` 1.5.1 + Espressif `esp32` core 3.3.12, sharing
`~/Library/Arduino15` with the Arduino IDE (so the same board shows up in both).

## Everyday commands

```sh
make ports     # what serial ports does macOS see?
make build     # compile only
make upload    # compile + flash
make monitor   # serial monitor @ 115200 (Ctrl-C to quit)
make flash     # upload then monitor
make clean

make intellisense   # rebuild VSCode include paths after a core/library change
make remote         # serve the phone remote at http://localhost:8000 (open in Chrome)
```

## Phone remote (Bluetooth)

[remote/index.html](remote/index.html) is a single page that talks Web
Bluetooth to the ESP32's Nordic UART service. It sends the same text commands
as the serial console and polls `APP`, which the firmware answers with one
`@S {json}` status line.

- **iPhone:** Safari can't do Bluetooth. Open the page in the free **Bluefy**
  browser. It must be loaded over `https://` (e.g. GitHub Pages).
- **Android / Mac / Windows:** Chrome. On a Mac, `make remote` then open
  http://localhost:8000.

Tap **Connect**, pick **RunCam-Ctl**, then **Wake camera** → **Start
recording** → **Stop & save**. The page always says what the camera's blue
light should be doing (solid = idle, slow blink = recording); if it doesn't
match, **Camera light doesn't match?** fixes it without pressing anything on
the camera (`SYNC`).

From VSCode: **Cmd-Shift-B** builds. Other targets are under
*Terminal → Run Task…* → `ESP32: Upload`, `ESP32: Upload + Monitor`, etc.

If auto-detect grabs the wrong port:

```sh
make upload PORT=/dev/cu.usbserial-10
```

## Connecting from your iPhone

1. Power the ESP32. It creates a Wi-Fi network:
   **`RunCam-Ctl`**, password **`runcam1234`** (change both in Settings).
2. iPhone: **Settings → Wi-Fi → RunCam-Ctl**. iOS will say *No Internet
   Connection* — that's expected; stay joined. Mobile data still works for
   other apps.
3. Open Safari at **http://192.168.4.1** (or **http://runcam.local**).
4. Optional: Share → **Add to Home Screen** for a full-screen app icon.

The page polls the ESP32 every second, so status stays live and several
phones can have it open at once.

| Button | Does |
|---|---|
| **Standby** | powers the camera and waits for it to be ready (~3 s) |
| **Start recording** | sends the RunCam start-record command |
| **Stop & save** | sends stop-record, then holds ~2.5 s while the file finalises |
| **Power off** | two taps; saves any recording first, then cuts power completely |

### HTTP API

| Method | Path | Does |
|---|---|---|
| GET | `/api/status` | JSON: state, recording time, battery, settings |
| POST | `/api/standby` | power on → STANDBY |
| POST | `/api/record` | start recording |
| POST | `/api/stop` | stop & save |
| POST | `/api/off` | save if needed, then cut power |
| POST | `/api/cmd` | any console command, form field `c` (e.g. `c=INFO`) |

Control actions return `{ok, msg, status}`; a refused action (e.g. record
before standby) returns HTTP 409 with the reason in `msg`.

States: `OFF` → `BOOTING` → `STANDBY` ⇄ `RECORDING` → `SAVING` → `STANDBY`,
and `STOPPING` (saving, then power off).

### Console commands

The same commands work in the page's Console panel and on the USB serial
console at 115200. Case-insensitive.

| Command | Does |
|---|---|
| `STANDBY` (or `ON`) | power on and get ready |
| `REC` / `STOP` | start / stop-and-save recording |
| `OFF` | cut power (saves any recording first) |
| `PWRBTN` | simulate the power button — camera standby, still powered |
| `WIFI` / `MODE` | simulate the camera's wifi button / change mode |
| `INFO` | probe the camera over UART, list the features it reports |
| `FRAMES` | dump the exact protocol bytes sent (CRC check / logic analyser) |
| `STATUS` / `RUNTIME` | state and battery / endurance estimate |
| `CAP <mAh>` | pack capacity |
| `WATTS <mW>` / `DRAW <mA>` | measured camera power, or derive it from a current reading |
| `CELLS <n>` | cells in series, 0 = auto-detect |
| `RESERVE <pct>` | reserve floor, default 15% |
| `SSID <name>` / `PASS <pw>` | Wi-Fi network name / password (8–63 chars) |
| `RESTART` | reboot to apply Wi-Fi changes (camera must be off) |

Settings persist in flash across power cycles. If a saved Wi-Fi setting is
ever rejected at boot, the ESP32 falls back to the defaults above so you
can't lock yourself out.

## Getting the video

The ESP32 only *controls* the camera — video is recorded to the Split 4's
own microSD card and never passes through the ESP32 (its link is far too
slow for video files). To get footage: power off from the page, take out
the microSD card, and read it with a card reader — on iPhone, a Lightning
or USB-C SD reader shows the files in the **Files** app.

## Two kinds of "off"

The page's **Standby** keeps the camera powered and ready. Separately,
`PWRBTN` asks the camera to go to its own standby — the board stays powered and keeps
drawing current. `OFF` opens a load switch and removes power completely, which
is the one that saves battery. `OFF` always stops recording and waits ~2.5 s for
the file to close first, because cutting power mid-write corrupts microSD cards.

## Gotchas on this board

- **Use a data USB-C cable.** Charge-only cables power the board but create no
  serial port — the single most common "it doesn't show up" cause.
- **Status LED (GPIO2):** slow pulse = off, fast blink = starting or saving,
  solid = standby, double flash = recording.
- **Hold BOOT if upload fails.** If you see
  `Failed to connect ... Wrong boot mode detected`, hold the **BOOT** button,
  start the upload, and release once "Connecting..." turns into writing.
- **GPIO6–11 are wired to the internal flash.** Never use them for I/O.
- **GPIO34–39 are input-only** — no `digitalWrite`, no pullups.
- **ADC2 pins don't work while Wi-Fi is on.** The battery sense is on GPIO34
  (ADC1) for that reason.
- `Serial` is the USB console at 115200. `Serial2` (GPIO16/17) talks to the
  camera and must not be used for anything else.
- **GPIO16/17 are free only on WROOM-32.** A WROVER module uses them for PSRAM.

## IntelliSense

`.vscode/c_cpp_properties.json` is generated, not hand-written — the ESP32 core
hides most of its ~360 include dirs behind gcc `@response` files, so the only
reliable source is a real compile command. After upgrading the core or adding a
library, run `make intellisense` to regenerate it.

## Adding libraries

```sh
arduino-cli lib search <name>
arduino-cli lib install "Adafruit NeoPixel"
```

Installed libraries land in the shared sketchbook, so the Arduino IDE sees
them too.
