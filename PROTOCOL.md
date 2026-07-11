# MHI PJZ IR protocol

Compact reference for the Mitsubishi Heavy Industries (MHI) PJZ-series IR
protocol, reverse-engineered from the **PJZ502A030D** hand remote. All bit
fields below were verified empirically against live captures (TSOP34836/38
receivers, raw + Pronto dumps).

> MHI is **not** Mitsubishi Electric — the protocols are unrelated. Within
> MHI, the PJZ family shares its timings with ToniA's `MitsubishiHeavyFDTC`
> (remote PJA502A704AA) but uses a longer 160-bit frame.

## Timing

38 kHz carrier, pulse-distance encoding. Each frame is sent **once** (no
repeat frame).

| Element | µs |
|---|---|
| Header mark | 6000 |
| Header space | 7500 |
| Bit mark | 500 |
| "0" space | 1500 |
| "1" space | 3500 |
| End pulse | 500 |
| End gap | 7500 |
| **Final mark** | 500 |

The trailing **final mark after the end gap is mandatory**: the AC treats the
end gap as a frame-internal long space and only accepts a frame terminated by
a mark. Without it the frame is silently discarded (verified on the unit).

## Status frame (160 bits)

Five 32-bit blocks, no separators. Bit numbering below is *protocol order*:
bit 0 = first bit on air = MSB of the block.

```
[B1] [B2 = ~B1] [B3] [B4 = ~B3] [B5 = static]
```

- B2/B4 are bitwise complements of B1/B3 (integrity).
- B5 is constant: `0x40BF0000` (`01000000 10111111 00000000 00000000`).

### Block 1 — main control

| Bits | Field |
|---|---|
| 0–13 | Customer code, fixed: `10110000000000` |
| 14 | Swing active: 1 = auto/swing on, 0 = fixed vane position |
| 15 | 0 |
| 16–19 | Target temperature, **LSB-first**: value + 16 = °C (range 18–30) |
| 20–22 | Mode: auto `000`, heat `001`, cool `010`, dry `100`, fan `110` |
| 23 | Power: 1 = on, 0 = off |
| 24–27 | 0 (in normal operation) |
| 28–31 | Vane position: auto `1100`, pos1 `0000`, pos2 `1000`, pos3 `0100`, pos4 `1100` |

Vane pos4 and auto share `1100`; bit 14 disambiguates. Power-off frames are
the last state with only bit 23 cleared — all other fields keep their values.
Dry/fan/auto modes still encode the real setpoint in bits 16–19.

### Block 3 — fan + feature flags

| Bits | Field |
|---|---|
| 0–2 | Fan: auto `001`, stage 1 `000`, stage 2 `100`, stage 3 `010`, stage 4 `110` |
| 15 | Silent flag |
| 22 | Hi-Power flag |
| 23 | Econo flag |
| rest | fixed |

Full B3 values (hex, fan in bits 0–2, no flags):

| Fan | B3 |
|---|---|
| auto | `0x20802800` |
| 1 (low) | `0x00802800` |
| 2 | `0x80802800` |
| 3 | `0x40802800` |
| 4 (high) | `0xC0802800` |

### Feature buttons (presets)

| Function | Encoding |
|---|---|
| Hi-Power | B1 temp bits 16–19 = `0000` (16 °C underflow code, below the 18 °C minimum) **and** B3 bit 22 = 1 |
| Econo | Setpoint forced to 28 °C (normal temp encoding) **and** B3 bit 23 = 1 |
| Silent | B3 bit 15 = 1 (`b3 |= 0x00010000`); everything else unchanged |

> An earlier hypothesis that Silent was a 7-bit XOR mask came from VS1838B
> demodulator drift and is wrong — a clean capture shows bit 15 only.

### Reference frame

Power on · Cool · 24 °C · fan auto · swing auto:

```
B1 = 0xB002150C   B2 = 0x4FFDEAF3
B3 = 0x20802800   B4 = 0xDF7FD7FF
B5 = 0x40BF0000
```

## Timer frame (191 bits)

Sent when programming the on/off timers. Different layout — **no customer
code** at the start:

```
Bits   0–11   OFF-timer time code (0 if OFF timer inactive)
Bits  12–23   ON-timer time code  (0 if ON timer inactive)
Bits  24–47   ~(bits 0–23)
Bits  48–79   BlockT1: 10110000 XXX 0010000100001010100001100
Bits  80–111  ~BlockT1
Bits 112–191  embedded AC status (same layout as the normal frame)
```

`XXX` (bits 56–58) is a one-hot timer-mode flag: `010` = ON timer only,
`100` = OFF timer only, `001` = both.

The 12-bit time code encodes a **time of day** (10-minute resolution) via a
Gray-code-based scheme over hour/minute digits:

```cpp
static uint8_t gray(uint8_t n) { return n ^ (n >> 1); }

uint16_t encode_timer_time(uint8_t H, uint8_t M) {  // H 0–23, M 0–50 step 10
  uint8_t h_tens = H / 10, h_ones = H % 10, m_tens = M / 10;
  bool Ho3 = (h_ones >> 3) & 1, Ho0 = h_ones & 1;
  bool Mt0 = m_tens & 1, Mt2 = (m_tens >> 2) & 1;
  uint8_t gHt = gray(h_tens), gHo = gray(h_ones), gMt = gray(m_tens);
  bool GHt0 = gHt & 1, GHt1 = (gHt >> 1) & 1, GHt2 = (gHt >> 2) & 1;
  bool GHo0 = gHo & 1, GHo1 = (gHo >> 1) & 1, GHo3 = (gHo >> 3) & 1;
  bool GMt0 = gMt & 1, GMt1 = (gMt >> 1) & 1, GMt2 = (gMt >> 2) & 1;

  uint16_t code = 0;
  code |= uint16_t(GHt0) << 11;
  code |= uint16_t(!GHt1) << 10;
  code |= uint16_t(GHt1) << 9;
  code |= uint16_t(GHt2) << 8;
  code |= uint16_t(GHo3 ^ GMt2) << 7;
  code |= uint16_t(!((Ho0 && !Ho3) || (Ho3 && Mt0) || Mt2)) << 6;
  code |= uint16_t(GHo0 && !GMt1) << 5;
  code |= uint16_t(GHo3) << 4;
  code |= uint16_t(!((GMt0 && !Ho3) || (GHo0 && !GHo1))) << 3;
  code |= uint16_t((!(Ho0 ^ Mt0)) ^ (Ho3 && GMt1)) << 2;
  code |= uint16_t(!(Ho3 && GMt0 && !GMt1)) << 1;
  code |= uint16_t(!(Ho3 ^ Ho0 ^ Mt0)) << 0;
  return code;  // verified against 11 captured data points
}
```

The component does **not** transmit timer frames (cancelling a timer is just
a normal 160-bit status frame). Received timer frames fail the customer-code
check and are ignored gracefully.

## Receiver / decoding notes

- Classify spaces with a **fixed ~2500 µs threshold**, not the receiver
  tolerance: at the 55 % tolerance needed for the long frame, the 1500/3500 µs
  windows would overlap.
- Do **not** hard-verify B2 = ~B1 / B4 = ~B3 on receive: demodulator drift
  flips single bits in the complement blocks of real captures. Check the
  customer code instead and ignore unknown bits.
- Use a native 38 kHz demodulator (TSOP34838). A TSOP34836 (36 kHz) also
  decodes reliably; the VS1838B does not — it shows progressive mark drift
  over the 160-bit frame and produced most of the artifacts noted above.

## References

- [joedirium/Mitsubishi_Heavy_HVAC_IR](https://github.com/joedirium/Mitsubishi_Heavy_HVAC_IR) — timing primary source (oscilloscope, PJA502A704AA)
- [ToniA/arduino-heatpumpir](https://github.com/ToniA/arduino-heatpumpir) — `MitsubishiHeavyFDTC`: same timing family, 64-bit frame
- [poolski/esphome-mitsubishi-with-remote](https://github.com/poolski/esphome-mitsubishi-with-remote) — architecture reference for simultaneous TX/RX
