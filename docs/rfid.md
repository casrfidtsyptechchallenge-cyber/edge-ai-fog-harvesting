# RFID badges (M4)

A badge proves someone is physically at the node before the high voltage may be used. Each node's MCX A156
reads cards with an RC522 reader, looks up the card's role in its badge list and hands the tap to the safety
core (M3), which decides and logs a `BADGE_TAP` event. The station owns the badge list and sends it to every
node (`SYNC_ALLOWLIST`), and each node keeps its own copy, so badges keep working while the Pi or the radio is down.

| Role | What a tap does at the node |
|---|---|
| operator, admin | arm (LOCKED to ARMED), or disarm; see docs/safety_core.md for every rule |
| technician | maintenance lock-out; only the same card ends it |
| household | denied at the node: water-point cards (M9, docs/water_point.md) |
| unknown | denied: any card that is not on the list |

## Node (firmware/node/lib/rfid, firmware/mcx_node)

- The RC522 is polled 10 times a second. A card left on the reader counts once; it counts again only after it
  has been away for 0.5 s. 4, 7 and 10-byte UIDs are read (MIFARE Classic, Ultralight, NTAG).
- The reader is a sensor like the others: it shows as `RC522 missing` when not connected, is looked for again
  every 5 s, and appears in the info message.
- At every start the badge list comes from `AEROION_LOCAL_BADGES` in `mcx_node_config.h` (at most 64 badges).
  A `SYNC_ALLOWLIST` from the station replaces it completely; a list older than the node's is refused.

Wiring (RC522 module to the FRDM-MCXA156 header). **3.3 V only, never 5 V.**

| RC522 | Header | MCX pin |
|---|---|---|
| SDA (chip select) | D9 | P3_17 |
| SCK | D13 | P2_12 |
| MOSI | D11 | P3_15 |
| MISO | D12 | P2_16 |
| RST | D5 | P3_14 |
| 3.3V / GND | 3V3 / GND | |
| IRQ | not connected | |

Console commands (serial terminal, type `help`):

| Command | Does |
|---|---|
| `tap <UID>` | as if that card was read (tests without a reader) |
| `op`, `tech` or SW2, SW3 | taps of the test badges 0A0B0C0D (operator) and 0E0E0E0E (technician) |
| `badges` | the node's badge list and where it came from |
| `allow <UID> <role>`, `forget <UID>` | change the node's list on the bench, until the station sends its list |
| `sensors` | shows `RC522 ok` and the chip version (0x91 or 0x92 for NXP chips; clones differ; 0xFF = no chip) |

## Station (station/badges)

The list is `data/badges.json`. Every change raises its version; run `sync` after changes.

```
python -m station.badges add 04A1B2C3 operator --label "white card"
python -m station.badges list
python -m station.badges remove 04A1B2C3
python -m station.badges check
python -m station.badges sync --node all            # through the MQTT broker; prints each node's answer
python -m station.badges sync --node n01 --print    # only print the command, one contract JSON line
```

## Testing

Without hardware:

1. PC tests: `python -m platformio test -e native` in `firmware/node`, and `python -m pytest -q` in the project folder.
2. MCX board, no reader: `sensors` shows `RC522 missing`. `tap 04A1B2C3` is denied (unknown);
   `allow 04A1B2C3 operator`, then `tap 04A1B2C3` arms the node. SW2/SW3 work as before.
3. Station with the simulator: start the broker (`mosquitto`), then `python sim/fake_node.py --broker localhost`,
   then `python -m station.badges sync --node all`: every simulated node answers `accepted`.

With an RC522: wire it as above and press RESET. `sensors` shows `RC522 ok`. Present a card: the node prints
`# badge <UID> (unknown): denied`. Add that UID with `allow` (bench) and in `data/badges.json` (station),
and for cards that must work even when the station is down, in `AEROION_LOCAL_BADGES`.

## Limits to state openly

- Cards are checked by their UID only (as planned for the prototype). Such cards can be cloned, so a UID works
  like a password: keep real UIDs out of public places (`data/` is in `.gitignore`). Real deployment:
  cryptographic cards such as NTAG 424 DNA or MIFARE DESFire.
- The node keeps the station's list in RAM. After a restart it starts again from `mcx_node_config.h` until the
  station sends the list again: run `python -m station.badges sync --node all` after a node restarts.
- `SYNC_ALLOWLIST` reaches the MCX through the station link (M7, `docs/link.md`): the node's ESP32, or
  `tools/serial_bridge.py` on the USB port.
- Household cards and the NTAG213 filter tag are read at the water point, on the Pi (M9): see
  docs/water_point.md. The filter tags are registered in the water point's ledger, not in this badge list.