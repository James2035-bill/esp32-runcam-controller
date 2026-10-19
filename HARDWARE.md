# Wiring: ESP32 DevKit (WROOM-32) -> RunCam Split 4

## Pin map

| ESP32 pin | Goes to | Notes |
|---|---|---|
| GPIO16 (RX2) | camera **TX** | crossed — RX to TX |
| GPIO17 (TX2) | camera **RX** | crossed — TX to RX |
| GPIO13 | load-switch enable | high = camera powered |
| GPIO34 | battery divider tap | input-only pin, ADC1 |
| GPIO2 | onboard LED | status indicator |
| GND | camera GND **and** battery GND | one common ground, always |
| VIN (or 5V) | 5 V from a BEC / buck converter | powers the ESP32 — see *Powering* below |

Camera side: the Split 4's UART pads are labelled **TX / RX / GND** on the
main board (check the silkscreen or RunCam's manual for your board revision).
Only three wires go between the boards for control: TX, RX and GND.

```
   ESP32 DevKit                         RunCam Split 4 (main board)
  ┌────────────┐                       ┌──────────────────────────┐
  │     GPIO16 ├◄──────────────────────┤ TX                       │
  │     GPIO17 ├──────────────────────►│ RX                       │
  │        GND ├───────────┬───────────┤ GND                      │
  │            │           │           │                          │
  │     GPIO13 ├──► load switch EN     │ VIN (5-20 V) ◄── switched │
  │     GPIO34 ├◄── battery divider    │                  pack +  │
  │        VIN ├◄── 5 V BEC ◄── pack + └──────────────────────────┘
  └────────────┘           │
                     pack − / GND (common)
```

## UART logic levels

The ESP32's GPIOs are **3.3 V and not 5 V-tolerant**. RunCam's split cameras
use 3.3 V TTL on their control UART — the same lines flight controllers drive
directly — so TX/RX normally connect straight through with no level shifter.

**Check it on your board before connecting**, because it's the one way to
damage the ESP32 here: power the camera alone, and measure the camera's **TX**
pad to GND with a multimeter. An idle UART line sits high:

| Camera TX idle reads | What to do |
|---|---|
| ~3.3 V | wire directly |
| ~5 V | add a divider on camera TX → GPIO16: **1 kΩ** in series, **2 kΩ** from GPIO16 to GND (gives ~3.3 V) |

The other direction (ESP32 TX 3.3 V → camera RX) is fine either way: 3.3 V is
a valid logic high for 3.3 V and for most 5 V TTL inputs.

Keep the UART wires short (under ~30 cm) and twisted with GND if they run
near motors or ESCs.

GPIO16/17 are free because WROOM-32 has no PSRAM. On a **WROVER** module those
two pins are wired to the PSRAM die and the UART must move (e.g. 25/26).

GPIO34 is input-only, which is what you want for a sense pin, and it is on
**ADC1**. ADC2 stops working whenever Wi-Fi is active, so this build must
sense on ADC1 (GPIO32–39).

## The two kinds of "off"

| Command | Mechanism | Current after | Use |
|---|---|---|---|
| `PWRBTN` | RunCam protocol, power-button simulation | still drawing | quick standby |
| `OFF` | opens the load switch | **zero** | saving battery |

`OFF` stops recording and waits ~2.5 s for the file to close before cutting
power. Pulling the rail mid-write is the standard way to corrupt a microSD
card and lose the clip.

## Load switch

GPIO13 cannot switch the camera directly — the camera draws far more than a
GPIO can source, and the rail is 5–20 V while the GPIO swings 0–3.3 V. Use a
**high-side P-channel MOSFET** with an NPN level-shifter:

```
  VBAT (5-20V) ──────┬──────────── S
                     │          P-MOSFET (e.g. AO3401, IRF9540)
                    [R1]           G ──┬──── D ───── camera VIN
                    100k               │
                     │                [R2] 10k
                     └─────────────────┤
                                       │
                              C of NPN (2N3904/BC547)
                                       │
                     GPIO13 ──[1k]── B │
                                       │
                                     E ┴ GND
```

GPIO13 high turns the NPN on, pulls the gate low, and the P-MOSFET conducts.
R1 keeps the gate at VBAT (MOSFET off) when GPIO13 is low or the ESP32 is in
reset — so the camera is off by default, which is the safe failure direction.

A ready-made alternative: any **high-side load-switch module** rated for your
pack voltage and ≥2 A, wired enable-to-GPIO13. Set `CAM_PWR_ACTIVE_HIGH` in
the sketch to match its polarity.

## Battery sense divider

```
  VBAT ──[ R1 100k ]──┬──[ R2 15k ]── GND
                      │
                   GPIO34
```

Divides by 7.67, so a 20 V pack presents 2.61 V at the pin — inside the ADC's
range with headroom. If you change the resistors, update `DIV_R1`/`DIV_R2` in
the sketch. The firmware uses `analogReadMilliVolts()`, which applies the
chip's factory ADC calibration, so no manual curve-fitting is needed.

## Powering the ESP32 — read this before connecting a big pack

The DevKit's VIN goes through an **AMS1117 regulator, rated ~15 V absolute
max**, and it is a *linear* regulator, so every volt above 3.3 becomes heat.

| Pack | How to power the ESP32 |
|---|---|
| 1S–2S (3.7–8.4 V) | VIN directly is fine |
| 3S (11.1 V) | works, but the regulator runs hot — a buck is better |
| 4S+ (14.8–20 V) | **do not use VIN.** Add a 5 V buck converter (MP1584, LM2596) feeding VIN or the 5V pin |

The camera itself takes 5–20 V on its own input, so above 3S the usual layout
is: pack → camera VIN (through the load switch), and pack → buck → ESP32.

**Using a 5 V BEC** (the usual choice on a drone or RC build): feed the BEC's
5 V into the DevKit's **VIN** pin (or **5V** — same net on most DevKit V1
boards) and its ground into **GND**. Size it for at least **500 mA** — Wi-Fi
transmit bursts pull ~250–350 mA and a weak supply causes brownout resets.
Don't power the ESP32 from USB and a BEC at the same time unless your board
has a diode on the USB 5 V line.

You *can* also run the camera from the 5 V BEC instead of the pack (it accepts
5–20 V), but then the BEC carries the camera's ~0.5 A as well — size it for
**2 A or more** and keep the load switch on that 5 V feed.

## Fallback: button-pad control (no UART)

If the camera never answers `INFO` (no UART reply even with TX/RX checked and
swapped), the UART on your board revision may be disabled or set to another
mode. The fallback is to simulate the camera's physical button: wire an
optocoupler (PC817) or NPN transistor across the button's two contacts, and
pulse it from a spare GPIO (e.g. GPIO25) for ~200 ms. That "presses" the
button exactly as a finger would.

This is not built into the firmware, on purpose: what each press does
(start/stop, mode change, power) depends on the camera's button mapping and
the current mode, and the ESP32 gets no feedback, so its idea of "recording"
can drift from reality. The UART protocol has distinct start and stop commands
and reports what the camera supports, which is why it's the default. If you
need the fallback, check which pads your board's button uses, and it can be
added as an option.

## Handling the flex cable

The Split 4's sensor head connects to the main board with a fine FPC ribbon.
Do not crease it, fold it sharply, or tug it — the traces crack and the
failure looks like a dead sensor. Leave a gentle loop, and if you must route
it through a frame, protect the edges. The UART control wires are soldered to
pads on the **main board**, never to the ribbon.

---

# How long can the camera stay on?

There is no single number, because it depends on two things: your pack's energy
and the camera's actual power draw. Here is the honest version of both.

## Why watt-hours, not mAh

The Split 4 takes **5–20 V**, which means it regulates internally and behaves as
a roughly **constant-power** load. As the pack sags, the current it pulls goes
*up*. So endurance is:

```
runtime_hours = (pack_Wh x usable_fraction) / (camera_W + esp_W)

pack_Wh = nominal_voltage x capacity_Ah      e.g. 11.1 V x 2.2 Ah = 24.4 Wh
```

Quoting the camera as "350 mA" is only meaningful alongside a voltage — the
same 2 W camera pulls 541 mA from 1S but only 180 mA from 3S.

## Estimates (15% reserve, ESP32 + Wi-Fi AP at 0.7 W)

| Pack | Wh | 1.0 W | 1.5 W | 2.0 W | 2.5 W | 3.0 W | 3.5 W |
|---|---|---|---|---|---|---|---|
| 1S 18650 3000mAh | 11.1 | 5h33 | 4h17 | 3h30 | 2h57 | 2h33 | 2h15 |
| 2S LiPo 1500mAh | 11.1 | 5h33 | 4h17 | 3h30 | 2h57 | 2h33 | 2h15 |
| 2S LiPo 2200mAh | 16.3 | 8h08 | 6h17 | 5h08 | 4h19 | 3h44 | 3h18 |
| 3S LiPo 2200mAh | 24.4 | 12h13 | 9h26 | 7h41 | 6h29 | 5h37 | 4h57 |
| 3S LiPo 5000mAh | 55.5 | 27h45 | 21h27 | 17h28 | 14h45 | 12h45 | 11h14 |
| 4S LiPo 1500mAh | 22.2 | 11h06 | 8h35 | 6h59 | 5h54 | 5h06 | 4h30 |

**Which column applies to you is the open question.** Recording 4K to microSD
is the dominant load in a camera like this, and split-style FPV cameras
generally land somewhere in the 1.5–3 W band — but I am not going to hand you a
precise figure for the Split 4 from memory. Measure it:

- **Multimeter in series** with the camera's supply, camera recording, and note
  the voltage at the same time. Then `DRAW <mA>` and the firmware converts it.
- **USB power meter** if you are running the camera from 5 V — it reads watts
  directly. Then `WATTS <mW>`.
- Or read the figure off RunCam's spec sheet for your exact model and use
  `WATTS`.

Measure while **recording**, not idling — idle draw is substantially lower and
will flatter the estimate.

## Things that shorten it

- **Recording** costs more than idle previewing.
- **The camera's own WiFi**, if you enable it, is a large extra load.
- **Cold** cuts usable LiPo capacity noticeably.
- **A linear regulator** feeding the ESP32 from a high pack voltage wastes the
  difference as heat. On 3S+, a buck converter is worth real runtime.

## Pack voltage ceiling

The camera's limit is **20 V**, and that is an absolute maximum, not nominal:

| Pack | Nominal | Full charge | Safe? |
|---|---|---|---|
| 3S | 11.1 V | 12.6 V | yes |
| 4S | 14.8 V | 16.8 V | yes |
| 5S | 18.5 V | **21.0 V** | **no — exceeds 20 V when full** |

So **4S is the largest pack you can wire straight to the camera.** Beyond that
you need a buck converter in front of it.
