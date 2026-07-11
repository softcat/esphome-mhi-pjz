# mhi_pjz — Mitsubishi Heavy Industries PJZ IR climate for ESPHome

ESPHome external component (`climate_ir::ClimateIR`) for **Mitsubishi Heavy
Industries** air conditioners controlled by **PJZ-series** IR remotes
(reverse-engineered from and tested with the PJZ502A030D).

> **MHI ≠ Mitsubishi Electric.** The stock ESPHome `mitsubishi` component
> speaks Mitsubishi *Electric* — a completely different protocol. Other MHI
> components (ToniA ZJ/ZM/ZMP, `esphome-climate-mhi`) target other remote
> families and are not compatible either. See [`PROTOCOL.md`](PROTOCOL.md).

## Features

- **TX**: power, mode (auto/heat/cool/dry/fan), target temperature 18–30 °C,
  fan (auto + 4 stages), vertical swing (auto/fixed)
- **RX**: decodes the original hand remote and keeps the Home Assistant
  climate entity in sync, including presets
- **Presets** mapped bidirectionally to HA:

  | AC function | HA preset | Encoding |
  |---|---|---|
  | Hi-Power | `BOOST` | 16 °C underflow code + B3 flag |
  | Econo | `ECO` | forces 28 °C setpoint + B3 flag |
  | Silent | `SLEEP` | B3 flag |

- **Self-echo suppression**: content-based — a received frame matching the
  last transmit within 2 s is recognized as the device's own IR reflection
  and ignored (no state flicker, no redundant publishes)

## Installation

As a published external component:

```yaml
external_components:
  - source: github://softcat/esphome-mhi-pjz@main
    components: [ mhi_pjz ]
```

Or vendored locally (path relative to the device YAML):

```yaml
external_components:
  - source:
      type: local
      path: components
    components: [ mhi_pjz ]
```

## Configuration

```yaml
remote_transmitter:
  id: ir_tx
  pin: GPIO17
  carrier_duty_percent: 50%
  non_blocking: true

# Receiver is optional — omit it (and receiver_id) for TX-only operation.
remote_receiver:
  id: ir_rx
  pin:
    number: GPIO16
    inverted: true        # TSOP output is active-low
    mode:
      input: true
      pullup: true
  tolerance: 55%          # generous: 160-bit frames drift; the decoder
  buffer_size: 20kb       # classifies spaces by fixed threshold instead

climate:
  - platform: mhi_pjz
    name: "Aircon"
    transmitter_id: ir_tx
    receiver_id: ir_rx
```

Hardware notes: 940 nm IR LED on a transistor driver for TX; for RX use a
native 38 kHz demodulator (TSOP34838; a TSOP34836 also works, a VS1838B does
not — see `PROTOCOL.md`, receiver notes).

## Fan mode mapping

The unit has 4 fan stages plus auto; ESPHome's climate vocabulary has no
clean 4-speed set (QUIET is taken by the Silent preset), so:

| HA fan mode | MHI stage |
|-------------|-----------|
| `low`    | 1 (low) |
| `medium` | 2 (medium-low) |
| `middle` | 3 (medium-high) |
| `high`   | 4 (high) |

Note: HA renders the buttons in ESPHome enum order — Low, Medium, High,
**Middle** — so Middle appears last even though it is stage 3 of 4. This is
a deliberate trade-off, not a bug.

## Limitations

- Fixed vane positions 2–4 are decoded as "swing off" but cannot be selected
  from HA (`swing_mode` has no position slots; would need a separate
  `select` entity).
- Timer frames (191-bit) are documented in `PROTOCOL.md` but not
  transmitted; incoming timer frames are ignored gracefully.
- IR is one-way — if the AC misses a frame, states drift silently (inherent
  to IR, no ACK channel).

## Protocol

Frame layout, bit fields, timings and receiver guidance:
[`PROTOCOL.md`](PROTOCOL.md).

## License

[MIT](LICENSE) · CODEOWNERS: `@softcat`
