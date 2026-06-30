# Ice Maker Panel Control Protocol

This document is the final working reference for future remote-control
development. It summarizes the reverse-engineered five-wire panel, the verified
state signatures, and the serial commands that simulate the original buttons.

The experimental direct-GPIO wiring has now been disconnected from the
ice-maker. The ESP32-C3 remains connected to the Mac only.

## Panel Netlist

The panel has five external nodes, `P1` through `P5`.

```text
P5-P1 : LED5 + R5      ice full
P5-P2 : LED4 + R4      no water
P1-P2 : SW1 + R6       power button
P1-P3 : SW2 + R7       select button
P3-P4 : R3 + LED3      large ice
P2-P4 : R2 + LED2      small ice
P1-P4 : R1 + LED1      power LED
```

The original controller multiplexes/scans these five nodes. In the direct
floating ESP32 setup, digital input masks were not useful, but ADC signatures
were repeatable enough for state recognition.

## ESP32 Mapping

The tested ESP32-C3 mapping was:

```text
P1 -> GPIO0 / ADC1_CH0
P2 -> GPIO1 / ADC1_CH1
P3 -> GPIO2 / ADC1_CH2
P4 -> GPIO3 / ADC1_CH3
P5 -> GPIO4 / ADC1_CH4
```

The firmware serial baud rate is `921600`.

## Important Hardware Boundary

The direct-GPIO method was an accepted-risk bench technique, not a production
interface. The panel can show 4.x V node differences while ESP32-C3 GPIOs are
3.3 V devices.

For a more robust remote-control build:

- Use isolated dry-contact devices such as PhotoMOS relays, reed relays, or
  optically isolated analog switches across the original button contacts.
- For state sensing, add high-value series resistors, clamps, and known biasing,
  or use an isolated logic analyzer/scope front-end.
- Do not rely on direct floating ADC for fault LEDs such as no-water and
  ice-full unless the limitation is acceptable.

## Scan Timing

The panel scan frame is approximately:

```text
13.03 ms per frame
76.7 Hz
```

The power LED standby blink is approximately:

```text
3.9-4.0 s period
about 0.25 Hz
```

## ADC Bucket Model

For direct floating ADC captures, each node is converted into a coarse bucket:

```text
0 : ADC < 100
H : ADC > 3995
M : ADC between about 1500 and 2500
x : anything else
```

The five-character signature is ordered as:

```text
P1 P2 P3 P4 P5
```

Examples:

```text
0HHHH : P1 low, P2/P3/P4/P5 high
MHMHH : P1/P3 mid, P2/P4/P5 high
```

## State Recognition

Use ADC mode at `adc_us 1000`. For reliable standby detection, collect at least
20-35 seconds so the 4-second power LED blink can be detected.

### Running, Large Ice

Signature:

```text
dominant bucket: 0HHHH
typical ratio: about 83%
P1 median: about 0
P3/P4/P5 median: about 4095
P1-P3 median: about -4095
no 4-second power LED blink
```

Observed and verified in:

```text
20260628-205853-direct_large_normal_adc
20260628-211027-direct_running_select_back_adc
20260628-213241-direct_sw2_sim_back_adc
20260628-221633-current_state_check_2_adc
```

### Running, Small Ice

Signature:

```text
dominant bucket: MHMHH
typical ratio: about 69-75%
P1-P3 median: near 0
P3-P4 median: about -2160 to -2200
no 4-second power LED blink
```

Important: small running mode and standby can both have `MHMHH` as the dominant
bucket. Distinguish them with the power LED blink test.

Observed and verified in:

```text
20260628-210541-direct_running_select_adc
20260628-211027-direct_running_select_back_adc
20260628-213241-direct_sw2_sim_back_adc
```

### Standby

Signature:

```text
dominant bucket: usually MHMHH
P1 and P1-P4 have a strong 3.9-4.0 s periodic component
visible behavior: power LED slow blink, other LEDs off
```

Recent classifier check:

```text
20260628-221419-current_state_check_adc
MHMHH ratio: about 74.5%
P1/LED1 peak: about 0.253 Hz, period about 3.94 s
classification: standby
```

### Fault LEDs

The direct floating method did not robustly separate these states:

```text
LED4 no water: P5-P2
LED5 ice full: P5-P1
```

No-water on/off changed ADC distributions around `P2` and `P5-P2`, but the
large-mode median signature remained almost identical. Treat no-water and
ice-full as unresolved in the direct-GPIO ADC method.

## Suggested Classifier

For future software, classify from a 20-35 second ADC capture:

```text
if ratio(0HHHH) > 0.65 and no strong 0.25 Hz blink:
    state = RUN_LARGE
elif ratio(MHMHH) > 0.55:
    if P1 or P1-P4 has a strong 0.18-0.35 Hz component:
        state = STANDBY
    else:
        state = RUN_SMALL
else:
    state = UNKNOWN
```

A practical blink score is the FFT power share in the `0.18-0.35 Hz` band for
0.5-second binned `P1` or `P1-P4`.

Do not classify standby from a very short capture; the 4-second blink must have
time to appear.

## Firmware Serial Commands

General commands:

```text
mode adc
mode edge
adc_us <200..1000000>
heartbeat_us <100000..10000000>
status
help
release
```

Button simulation commands:

```text
sw1_od 100          power button short press
sw2_od 80           select button short press
sw2_hold_od 5000    select button long press, toggles UV mode
```

Additional experimental commands exist:

```text
sw1_weak [ms]
sw1_hold_od [ms]
sw1_pair_od [ms]
sw2_weak [ms]
sw2_pair_od [ms]
```

Only the three commands in the first button table are recommended for future
remote-control logic.

## Verified Controls

### Power Button

Command:

```text
sw1_od 100
```

Electrical action:

```text
P2/GPIO1 open-drain low for 100 ms
then restore all panel pins to floating inputs
```

Verified behavior:

```text
running large -> standby
```

Evidence:

```text
20260628-213639-direct_sw1_sim_power_adc
```

Note: the same physical power button starts the machine from standby. The active
direct-GPIO `sw1_od 100` command is expected to do the same, but the logged
active test in this session only verified running-to-standby.

### Select Button

Command:

```text
sw2_od 80
```

Electrical action:

```text
P3/GPIO2 open-drain low for 80 ms
then restore all panel pins to floating inputs
```

Verified behavior:

```text
large -> small
small -> large
```

Evidence:

```text
20260628-212952-direct_sw2_sim_retry_adc
20260628-213241-direct_sw2_sim_back_adc
```

### UV Toggle

Command:

```text
sw2_hold_od 5000
```

Electrical action:

```text
P3/GPIO2 open-drain low for about 5 seconds
then restore all panel pins to floating inputs
```

Verified behavior:

```text
UV off -> UV on
UV on -> UV off
```

Evidence:

```text
20260628-214710-direct_sw2_hold_5s_uv_adc
20260628-214855-direct_sw2_hold_5s_uv_off_adc
```

The panel LEDs do not indicate UV state. A remote program cannot infer UV
state from the panel ADC signature alone. Keep UV state in software only if the
controller is the sole actor, or add an external UV-current/light sensor.

## Serial Output

The firmware emits:

```text
A,<us>,<p1_raw>,<p2_raw>,<p3_raw>,<p4_raw>,<p5_raw>,<mask>
E,<us>,<mask>
H,<us>,<mode>,<mask>
P,<us>,<action>,<ms>
```

`A` rows are ADC samples. `P` rows mark active simulated button pulses.

In the direct floating setup, `mask` was always `0`, so future logic should use
ADC rows, not digital masks.

## Remote-Control State Machine

Recommended high-level operations:

```text
get_state():
    collect ADC for 20-35 s
    classify as STANDBY, RUN_LARGE, RUN_SMALL, or UNKNOWN

power_off():
    if state is RUN_LARGE or RUN_SMALL:
        send "sw1_od 100"
        wait 5-10 s
        verify STANDBY with blink-aware classifier

power_on():
    if state is STANDBY:
        send "sw1_od 100"
        wait 10-20 s
        verify RUN_LARGE or RUN_SMALL
    note: active power-on should be verified after final hardware is installed

set_size(target):
    ensure machine is running
    if current size differs from target:
        send "sw2_od 80"
        wait 1-2 s
        verify target size

toggle_uv():
    send "sw2_hold_od 5000"
    wait 1-2 s
    update software UV state
```

Because UV has no panel feedback, do not offer an idempotent `set_uv(on/off)`
unless there is persistent software state or an external sensor.

## ESPHome Implementation

The Home Assistant firmware lives in:

```text
ice_panel_esphome/
```

It uses ESPHome Native API, a local `external_components` component named
`ice_panel`, and the same direct GPIO mapping verified during the bench tests:

```text
P1 -> GPIO0 / ADC1_CH0
P2 -> GPIO1 / ADC1_CH1
P3 -> GPIO2 / ADC1_CH2
P4 -> GPIO3 / ADC1_CH3
P5 -> GPIO4 / ADC1_CH4
```

The component samples P1-P5 at 1 kHz and keeps 64 half-second bins, giving a
32-second rolling signature window. The exported Home Assistant entities are:

```text
select Mode        Off / Small Ice / Large Ice
button UV Toggle   sends a 5 s Select hold; no UV feedback is claimed
text State         standby/running_large/running_small/starting/stopping/unknown
diagnostics        ADC signature, confidence, blink score, ratios, raw P1-P5
```

`Mode` is the main control surface. `Off` sends the SW1 power short press from a
running state. `Small Ice` and `Large Ice` send SW2 select pulses when already
running. If the current state is `standby`, selecting `Small Ice` or `Large Ice`
first sends SW1 to start the machine, then sends SW2 after startup if the
classified size differs from the requested target. Unknown states are refused
and logged rather than blindly pulsing the panel.

Build and serial settings:

```text
ESPHome version tested: 2026.6.2
Board: esp32-c3-devkitm-1
Framework: Arduino
Logger: 921600 baud, DEBUG level, USB_SERIAL_JTAG
Compile result: success
```

The local `ice_panel_esphome/secrets.yaml` contains placeholder Wi-Fi
credentials and random API/OTA keys so serial-only builds can compile. Replace
the Wi-Fi SSID/password and Home Assistant keys before production use.

If USB flashing fails with `Failed to connect to ESP32-C3: No serial data
received`, put the Super Mini into download mode manually:

```text
hold BOOT -> tap RESET -> release BOOT
```

If there is no RESET button:

```text
hold BOOT -> reconnect USB -> release BOOT
```

Then rerun:

```sh
.venv-esphome/bin/esphome upload ice_panel_esphome/ice-maker.yaml --device /dev/cu.usbmodem11301
```

## Recommended Production Hardware

Best remote-control topology:

```text
ESP32 GPIO -> isolated relay/PhotoMOS input
isolated output -> across original panel button contacts
```

Use one isolated output for:

```text
SW1 power button
SW2 select button
```

This emulates the original button contacts and avoids directly tying ESP32 GPIO
to the scanned panel nodes.

For state acquisition, either keep the current ADC signature approach with a
proper protection/bias front-end, or add independent sensors for:

```text
power LED
small LED
large LED
no-water LED
ice-full LED
UV indicator or UV lamp current
```

Independent optical/current sensors are more reliable for production than
interpreting the multiplexed five-wire panel electrically.
