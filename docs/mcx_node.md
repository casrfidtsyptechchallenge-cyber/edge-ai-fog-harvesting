# MCX node: sensing (M1), safety (M3), badges (M4), station link (M7) and arc predictor (M5a) on the NXP MCX A156

Each node has two chips (architecture v2). `firmware/mcx_node/` is the firmware of the **MCX A156**, the
safety controller: it reads the fast sensors (M1) and runs the safety core (M3). The ESP32-S3
(`firmware/node/`) keeps the radio, the AI models and the slow sensors (SHT45, load cell).

The sensor drivers in `firmware/node/lib/drivers/` are portable C++: they reach the hardware only through
`board_io.h`. The MCX implements it in `firmware/mcx_node/source/mcx_io.c`; the PC tests implement it with a
fake board (`firmware/node/test/test_drivers`). The safety core, sensor registry and message code are the
same files as on the ESP32.

## Wiring (Arduino header of the FRDM-MCXA156)

| Signal | Header | MCX pin | Notes |
|---|---|---|---|
| Collector current (op-amp output) | A0 | P1_10 (ADC1 channel 8) | 0 to 3.3 V |
| Photodiode, or the fog knob's wiper | A1 | P2_5 (ADC1 channel 1) | 0 to 3.3 V |
| Fog LED (through its driver) | D2 | P3_1 | output |
| Radar OUT (LD2410) | D4 | P3_31 | pull-up: a loose wire reads "person present" |
| Lid switch, other side to GND | D8 | P1_15 | pull-up: a loose wire reads "lid open" |
| INA219 SDA / SCL | D14 / D15 | P0_16 / P0_17 | address 0x40; the board's temperature sensor is at 0x48 |
| UART to the ESP32 (M7) | D0 (RX) / D1 (TX) | P2_11 / P2_10 | LPUART2; see docs/link.md |

Analog inputs take **0 to 3.3 V only**. Kept free for later: D0/D1 (UART to the ESP32, M7), D6/D7 (HV level
and enable), D11 to D13 (RFID reader, M4). D3 and D10 also drive the board's red and green LED.
Pins, calibration and the list of installed sensors are in `firmware/mcx_node/source/mcx_node_config.h`.

## Build and flash

Same as the M3 bench, with `mcx_node` instead of `mcx_bench`:

    cd C:\nxp\mcuxsdk
    python -m west build -b frdmmcxa156 <repo>\firmware\mcx_node -d C:\nxp\build\mcx_node -p always
    python -m west flash -d C:\nxp\build\mcx_node

Serial terminal: the MCU-Link port, 115200 baud. Type `help`.
To reach the station over MQTT without an ESP32, use python tools/serial_bridge.py --port COM5 instead of a terminal (docs/link.md): it shows the same lines and passes what you type to the board.

## Bench test with jumper wires (and one potentiometer if you have one)

| Step | Do | Expect |
|---|---|---|
| 1 | Nothing wired; type `i2c` | `i2c device at 0x48` (the board's own sensor): I2C works |
| 2 | Press SW2 | `denied ... cannot arm: lid open`: loose wires fail safe |
| 3 | A0, A1, D4 and D8 to GND; press RESET (the current zero is measured at boot) | sensors `ok` |
| 4 | Press SW2 | ARMED, green LED |
| 5 | A1 to 3V3 instead of GND (or a knob on A1, turned past one third) | FOG_DETECTED, HARVEST, blue LED |
| 6 | Touch A0 to 3V3 for less than a second | ARC, BACKOFF (blue blinking), HARVEST again 4 s later; 3 arcs within a minute give FAULT HV_OVERCURRENT |
| 7 | Remove the D4 wire | SAFE (person), red; put it back: ARMED, then HARVEST |
| 8 | Remove the D8 wire | FAULT LID_OPEN, SAFE; put it back, press SW2: ARMED |
| 9 | Hold A0 on 3V3 for 2 s | ARC, then FAULT HV_OVERCURRENT, SAFE |
| 10 | Press SW3 twice | MAINTENANCE (yellow), then LOCKED |
| 11 | `sensors`, then `zero` | states and raw readings; zero measured again (refused while the HV is allowed) |

Check the messages against the contract, as for any node:

    python tools/serial_capture.py --port COM3 --out mcx.jsonl --seconds 120
    python tools/contract_check.py --no-clock mcx.jsonl

## Limits to state openly

- `hv_on` means "allowed by the safety core": the HV output on this chip comes with the next step.
- The current is read 10 times a second, as an average of 32 conversions. Short arcs (microseconds to
  milliseconds) are caught by the arc predictor's fast windows (M5a), which cover 25.6 ms of every 100 ms:
  see `docs/edge_ai.md` (commands `ai`, `wave`, `waves`, and a bench test).
- The fog knob is a bench stand-in. Its fog index is reported with the source `optical`.
- Analog readings use the 3.3 V supply as their reference: measure it and set `ADC_FULL_SCALE_MV`.