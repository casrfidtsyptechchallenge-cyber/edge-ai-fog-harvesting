# Station link (M7)

The station reaches each node through MQTT. On the node, the **ESP32-S3** has the Wi-Fi and talks to the
broker; the **MCX A156** (safety controller) talks to its ESP32 over a UART. Both directions carry the same
JSON lines, one message per line, so the MCX does not know or care what is on the other end:

- the node's ESP32-S3 on D0/D1 (part 2, below), or.
- a PC running `tools/serial_bridge.py` on the MCX's USB port. It does the ESP32's job, so the whole
  chain can be tested with the MCX board alone.

```
station tools ── MQTT broker ── ESP32-S3 (Wi-Fi) ── UART D0/D1 ──┐
                            └── PC: tools/serial_bridge.py ── USB ┴── MCX A156: safety core, badges
```

## The lines

| Direction | Line | Meaning |
|---|---|---|
| MCX to link | `{"topic":"aeroion/n01/telemetry","retain":false,"payload":{...}}` | every contract message of the node: telemetry, event, status, info |
| MCX to USB only | `# state ARMED (operator badge)` | lines for people start with `#` |
| link to MCX | `{"topic":"aeroion/n01/cmd","retain":false,"payload":{...}}` | a station command (also `aeroion/all/cmd`) |
| link to MCX | `{"link":"time","ts":1791747040123}` | Unix time in ms, every 5 s |
| both, Wokwi only | `{"link":"radio","hex":"AE0101..."}` | a FOG_ALERT radio packet (part 3); the MCX ignores it |

- **Commands.** The MCX checks each command and always answers with one `CMD_ACK` event (`docs/safety_core.md`
  has the safety rules):

  | Command | On the MCX |
  |---|---|
  | `LOCK` | LOCKED now (refused in MAINTENANCE: only the technician's badge ends it) |
  | `PREARM` | lower fog threshold until `eta_s` + 60 s; needs ARMED. `eta_s` counts from the command's `ts` |
  | `SET_HINT` | the HV level the node asks for, until the command expires; the safety core still limits it |
  | `SYNC_ALLOWLIST` | replaces the badge list (M4); an older version is refused |
  | `TARE` | refused: the load cell is on the ESP32, which will answer TARE itself |

- **Expiry.** A command older than `ts` + `ttl_s` is answered `expired` and not run. The MCX can only tell
  once its clock is synced; before that it runs the command. Running one twice does no harm (MQTT may resend).
- **Time.** The MCX starts from its build time (`time_synced: false` in its status). The first time line sets
  its clock; the status then says `time_synced: true`.
- **Link up, link lost.** The link is up while time lines arrive. When it comes up, the MCX sends `info` and
  `status` again (the broker may have missed the ones from boot). After 15 s without a time line it sends one
  `FAULT COMM_LOST` event. Badges, fog and arcs keep working, because they are decided on the MCX. Only the
  station's commands stop arriving.
- **Last will.** If the bridge (or later the ESP32) disappears, the broker publishes the retained status
  `{"online": false}` for the node.
- **Limits.** Lines up to 4095 characters: a 64-badge `SYNC_ALLOWLIST` is about 3.5 kB. A longer line is
  dropped whole and the MCX says so.

## The ESP32-S3 (part 2)

The node's ESP32-S3 runs `firmware/node/src/link/` (PlatformIO envs `esp32s3-link` and `esp32s3-link-wokwi`;
the one-chip firmware of M1 to M3, env `esp32s3`, is unchanged). It does what `tools/serial_bridge.py` does,
plus:

- **One `seq` for the node.** The ESP32 numbers every telemetry and event it publishes, its own and the
  MCX's, so the station sees one counter per node (gaps = lost messages).
- **Its own measurements merged in.** The load cell (`mass_g`, `rate_gpm`) and the SHT45 (`t_c`, `rh_pct`)
  fill the MCX's nulls in the telemetry; the status gets `rssi_dbm` and `buffered_msgs`; the info lists the
  sensors and versions of both chips (`fw`: `0.4.0-mcx+0.5.0-esp`).
- **Offline buffer.** While the broker cannot be reached: every event and one telemetry in 30 are kept (64
  messages, the oldest telemetry goes first when full) and sent 2 s after the reconnect with
  `"buffered": true`, after the latest info and status. One `FAULT COMM_LOST` records the outage.
- **Commands.** TARE is done here (the load cell is on the ESP32). The others go to the MCX; if the MCX has
  said nothing for 5 s, or does not answer within 3 s, the ESP32 answers `rejected` itself, so the station
  always gets a CMD_ACK. If the MCX goes silent, one `FAULT COMM_LOST` says so.
- **Time.** NTP (`pool.ntp.org`, then the broker's host). The ESP32 sends the time to the MCX every 5 s, only
  once it has it: a wrong time is worse than none.
- **Last will** on `aeroion/<node>/status`: `online: false` if the ESP32 disappears.
- On its USB port: a log (`#` lines) and the commands `status`, `tare`, `health` and `clean` (the electrode
  health and virtual fog sensor it runs on the MCX's telemetry, M5: `docs/edge_ai.md`), and `m6` (the HV
  controller, which asks the MCX for the level: `docs/controller.md`).

Wiring (the same on the bench and in the box):

| MCX header | MCX pin | ESP32-S3 DevKitC-1 |
|---|---|---|
| D1 (TX) | P2_10, LPUART2 | GPIO 18 (UART1 RX) |
| D0 (RX) | P2_11, LPUART2 | GPIO 17 (UART1 TX) |
| GND | GND | GND |
| | | HX711: DT on GPIO 4, SCK on GPIO 5; SHT45: SDA 8, SCL 9 |

TX goes to RX each way. Both chips use 3.3 V logic, so the wires connect directly. Speed: 115200 baud on
both (`LINK_BAUD`). With nothing connected, D0 has a pull-up and reads idle. Settings of the ESP32:
`firmware/node/src/link/link_config.h` (`LINK_NODE_ID` must equal the MCX's `NODE_ID`).

### Test in Wokwi (no ESP32 board)

In `firmware/node/wokwi/link/`, Wokwi's serial monitor is wired to GPIO 17/18, the UART that goes to the MCX,
and shared on TCP port 4000. `tools/mcx_sim.py` plays the MCX there. The ESP32 reaches mosquitto on your PC as
`host.wokwi.internal`, through the IoT gateway built into Wokwi for VS Code.

1. `mosquitto -v` (the default local-only mode is enough for Wokwi).
2. In `firmware/node`: `python -m platformio run -e esp32s3-link-wokwi`
3. VS Code: F1 > "Wokwi: Select Config File" > `firmware/node/wokwi/link/wokwi.toml`, then F1 > "Wokwi: Start
   Simulator". Keep the simulator tab visible (a hidden tab pauses the simulation).
4. Right after: `python tools/mcx_sim.py` (default port `rfc2217://localhost:4000`). Within about 10 s it shows
   the ESP32's log (`MQTT connected`, `clock synced`) and `[mcx] time from the ESP32`; a status summary
   follows every 60 s.
5. `python tools/contract_check.py` in another terminal, then play the station as in part 1:

       python tools/send_cmd.py --node n01 lock "wokwi"        -> accepted (by the simulated MCX)
       python tools/send_cmd.py --node n01 tare                -> accepted (by the ESP32: the load cell)
       python -m station.badges sync --node all                -> n01: accepted

   Click the load cell in Wokwi and move its weight slider: `mass_g` changes in the telemetry (its scale
   depends on `HX711_COUNTS_PER_GRAM`, set for your own load cell); `send_cmd.py ... tare` brings it back to 0.
   If the log says `MQTT: no answer from host.wokwi.internal:1883 (state -2)`, start mosquitto with the
   `mosquitto.conf` of the real-board steps below.
6. Stop mosquitto for 20 s and start it again: the log shows `MQTT connection lost`, then `MQTT connected,
   N buffered message(s)`; `contract_check` shows events with `"buffered": true`.
7. Stop `mcx_sim.py`: 5 s later `FAULT COMM_LOST` (MCX silent), and commands are answered `rejected`.

**Your real MCX with the simulated ESP32:** `python tools/mcx_sim.py --relay COM5` (the MCX board's port):
only JSON lines cross, the `#` lines of both chips are shown, and what you type goes to the MCX's console.

### Real ESP32-S3 board

1. Copy `firmware/node/include/wifi_secrets.example.h` to `wifi_secrets.h` (same folder) and fill in your
   Wi-Fi and the broker's IP address. The file is in `.gitignore`.
2. mosquitto must accept other machines: a file `mosquitto.conf` with the two lines `listener 1883` and
   `allow_anonymous true`, then `mosquitto -c mosquitto.conf -v`, and allow port 1883 in the PC's firewall
   (private network). Bench only: a real site needs passwords and TLS.
3. In `firmware/node`: `python -m platformio run -e esp32s3-link -t upload`, then
   `python -m platformio device monitor`: `Wi-Fi connected`, `MQTT connected`, `clock synced`, and
   `MCX (safety controller) found on the link` once the three wires are in place.
4. The same station commands as in Wokwi. On the MCX's own serial port, `link` shows `link up`.

## FOG_ALERT between nodes (part 3)

Fog reaches the nodes one after the other. When one node detects it, its neighbours get ready (PREARM)
before it arrives, by radio, without the station or the broker.

1. The MCX sends `FOG_DETECTED`. The ESP32 publishes it as usual and broadcasts a FOG_ALERT by ESP-NOW: 16
   bytes (`contract/binary/fog_alert.json`), with the node's number, the detection time, an alert counter,
   the fog index and two flags. A broadcast has no acknowledgement, so each alert goes out 3 times, 100 ms
   apart. At most one alert per 10 s, so fog flickering at the threshold does not flood the air.
2. A neighbour's ESP32 hears it and gives its own MCX a PREARM, the same command the station sends:
   `cmd_id` `fa-n02-5` (alert 5 of node n02), `eta_s` 60, `source_node` `n02`, `ttl_s` 60. When both clocks
   are synced, `ts` is the detection time, so `eta_s` counts from the moment the neighbour saw the fog.
3. The MCX treats it like any PREARM (accepted only in ARMED: fog threshold 0.30 → 0.20) and answers with a
   CMD_ACK. That CMD_ACK reaches the station, so the station sees which node pre-armed which.

The ESP32 ignores its own alerts, the copies, other ESP-NOW packets, alerts more than 60 s old (when both
clocks are synced), and a second PREARM within 30 s. While the MCX is silent, alerts are only logged. Each
of these cases is a `#` line in the log. `status` on the USB console counts the alerts sent and heard.
The timings are in `GatewayConfig` (`lib/gateway/node_gateway.h`). The tests are in `test/test_fog_alert`;
one checks that the bytes are the same as Python's `pack_fog_alert`.

### Test in Wokwi

Wokwi simulates one board, so no neighbour can hear it by radio. The Wokwi build therefore also writes each
FOG_ALERT on the link as `{"link":"radio","hex":"..."}`, and reads the same kind of line as a packet heard by
radio. `tools/mcx_sim.py` shows these lines and plays the neighbour:

1. Run steps 1 to 4 of the Wokwi test above. When the simulated MCX detects fog, mcx_sim shows
   `[radio] FOG_ALERT broadcast by n01: fog 34 %, seq 1, ...`.
2. Type `alert n02 0.6` in mcx_sim. The ESP32 logs `# FOG_ALERT from n02 (fog 60 %): PREARM sent to the MCX`,
   the MCX gets PREARM `fa-n02-1`, and its CMD_ACK reaches the broker:
   `mosquitto_sub -t "aeroion/+/event" -v` (accepted in ARMED, otherwise rejected with the state).
3. Type `alert n03` within 30 s: `already pre-armed by a recent alert`. `alert n01` (this node) is refused.

The `alert` command works in relay mode (`--relay COM5`) too. The radio lines never reach the real MCX.

### Two real boards

Two nodes, each with its ESP32-S3 and MCX: give them different ids (n01, n02) in both
`mcx_node_config.h` and `link_config.h`, and put both on the same Wi-Fi network (ESP-NOW uses the
channel of the access point). Arm n01 (operator badge), then turn the fog knob of n02 up: n02 logs
`# FOG_ALERT to the neighbours: seq 1, fog 50 %`, and n01 logs `# FOG_ALERT from n02 (fog 50 %): PREARM
sent to the MCX`, followed by an accepted CMD_ACK `fa-n02-1` on the broker.


## Test without an ESP32 (MCX board on USB)

1. Flash `firmware/mcx_node` (see `docs/mcx_node.md`). Close any serial terminal.
2. Start the broker: `mosquitto -v` (or the Mosquitto service).
3. Start the bridge, with the MCU-Link port (`python tools/serial_bridge.py --list` shows the ports):

       python tools/serial_bridge.py --port COM5

   Within 5 s: `# station link up: the clock is synced`. Typing `link` shows the link, the clock and the
   answers so far. Every other console command works as before (`help`, `badges`, `op`, `state`...).
4. In another terminal, watch and check the traffic: `python tools/contract_check.py`
5. In a third terminal, play the station:

       python tools/send_cmd.py --node n01 lock "bench test"          -> n01: accepted
       python tools/send_cmd.py --node n01 prearm --eta 20 --source n02  -> rejected while LOCKED; arm (SW2), then accepted
       python tools/send_cmd.py --node n01 hint 0.4                   -> accepted; telemetry hv_level stays at or below 0.4 in HARVEST
       python tools/send_cmd.py --node n01 tare                       -> rejected (the ESP32's job)
       python -m station.badges sync --node all                       -> n01: accepted; type badges in the bridge

6. Stop the bridge (Ctrl-C): `contract_check` shows `status` with `online: false`. Open a serial terminal
   on the port: about 15 s after the stop, the board prints `FAULT COMM_LOST`. Close the terminal and start
   the bridge again: link up, `info` and `status` again.

Without a broker, a command can also be pasted straight into a serial terminal as one line, e.g. the output of
`python -m station.badges sync --node n01 --print`.

## Files

| File | What |
|---|---|
| `firmware/node/lib/link/` | portable C++, tested on the PC (`test/test_link`): line splitting, clock, commands, link watch |
| `firmware/mcx_node/source/mcx_uart.c` | the two UARTs of the MCX, with interrupts and ring buffers |
| `firmware/mcx_node/source/main.cpp` | sends every message to both ports, routes incoming lines |
| `tools/serial_bridge.py` | the PC bridge (`tests/test_serial_bridge.py`) |
| `firmware/node/lib/gateway/` | the ESP32's decisions, tested on the PC (`test/test_gateway`): merge, seq, offline buffer, answers |
| `firmware/node/src/link/` | the ESP32-S3 firmware: Wi-Fi, MQTT (PubSubClient), UART to the MCX, load cell, SHT45 |
| `firmware/node/wokwi/link/` | the Wokwi project of that firmware |
| `tools/mcx_sim.py` | the MCX end of the UART for Wokwi: simulated, or relayed to the real board; `alert` plays a neighbour (`tests/test_mcx_sim.py`) |
| `firmware/node/lib/gateway/fog_alert.*` | FOG_ALERT bytes and the filter for copies (`test/test_fog_alert`) |

## Limits to state openly

- The link is not encrypted or signed: anyone on the UART wires, or with access to the broker, can send
  commands. The safety core still has the last word (a command can lock or hint, never force the HV on).
  Real deployment: MQTT over TLS with a password per node.
- The MCX keeps no queue: a message sent while the ESP32 itself is down is lost for the station.
- The offline buffer is in RAM (64 messages): an ESP32 restart loses it. Telemetry while offline is thinned
  to one message in 30, so the station sees `seq` gaps for the others.
- MQTT messages go out with QoS 0 (PubSubClient): a message is lost if the connection breaks while it is on
  its way. Commands are received with QoS 1.
- Without NTP (no Internet and no NTP server at the station) the ESP32 sends no time: the MCX reports
  `time_synced: false` and, after 15 s, `COMM_LOST`, although commands still pass.
- FOG_ALERT is neither authenticated nor encrypted: anyone in radio range with an ESP32 can pre-arm the nodes.
  A PREARM only lowers the fog threshold of an armed node for a while; it cannot switch the HV on when the
  safety core says no. Field version: ESP-NOW encryption (a key per peer) or a signed packet.
- One hop: a node pre-arms only the neighbours in its own radio range, and alerts are not passed on.
- All nodes must be on the same Wi-Fi channel. The 3 copies make a lost alert unlikely, not impossible.