#pragma once

#include "esphome/components/climate_ir/climate_ir.h"

namespace esphome {
namespace mhi_pjz {

// ─── IR timing (µs), 38 kHz carrier ──────────────────────────────────────────
// Verified by IR capture; see PROTOCOL.md (in this directory) for the full protocol notes.
// 38 kHz confirmed: same timings and frequency as ToniA MitsubishiHeavyFDTC
// (remote PJA502A704AA); the PJZ series belongs to the same MHI protocol family.
static const uint32_t MHI_CARRIER = 38000;
static const uint32_t MHI_HEADER_MARK = 6000;
static const uint32_t MHI_HEADER_SPACE = 7500;
static const uint32_t MHI_BIT_MARK = 500;
static const uint32_t MHI_ZERO_SPACE = 1500;
static const uint32_t MHI_ONE_SPACE = 3500;
static const uint32_t MHI_END_PULSE = 500;
static const uint32_t MHI_END_GAP = 7500;
// Space classification threshold (between 1500 and 3500). The receiver runs at
// 55% tolerance which overlaps for spaces, so we classify by a fixed threshold.
static const uint32_t MHI_SPACE_THRESHOLD = 2500;
// A frame matching our last transmit, received within this window, is our own
// LED echo (a 160-bit frame is ~500 ms of airtime, so the echo arrives well
// after the transmit starts). Suppress it for this long.
static const uint32_t MHI_SELF_RX_SUPPRESS_MS = 2000;
// Silent preset: Block 3 protocol bit 15 (uint32 bit 16). Confirmed by clean
// TSOP34836 capture (normal 0x20802800 → silent 0x20812800). The 7-bit mask in
// older notes was a VS1838B drift artifact.
static const uint32_t MHI_SILENT_FLAG = 0x00010000u;

static const uint8_t MHI_NBITS = 160;  // 5 × 32-bit blocks
static const uint8_t MHI_NBLOCKS = 5;

// Temperature range (°C)
static const float MHI_TEMP_MIN = 18.0f;
static const float MHI_TEMP_MAX = 30.0f;

class MhiPjzClimate final : public climate_ir::ClimateIR {
 public:
  // Fan stages 1–4 map to LOW / MEDIUM / MIDDLE / HIGH: MEDIUM = stage 2
  // (medium-low), MIDDLE = stage 3 (medium-high). Deliberate: QUIET is taken
  // by the Silent preset and ESPHome has no fourth plain speed. HA lists the
  // buttons in enum order (Low, Medium, High, Middle), so Middle appears last
  // even though it is stage 3 of 4.
  MhiPjzClimate()
      : climate_ir::ClimateIR(MHI_TEMP_MIN, MHI_TEMP_MAX, 1.0f, true, true,
                              {climate::CLIMATE_FAN_AUTO, climate::CLIMATE_FAN_LOW,
                               climate::CLIMATE_FAN_MEDIUM, climate::CLIMATE_FAN_MIDDLE,
                               climate::CLIMATE_FAN_HIGH},
                              {climate::CLIMATE_SWING_OFF, climate::CLIMATE_SWING_VERTICAL},
                              {climate::CLIMATE_PRESET_NONE, climate::CLIMATE_PRESET_BOOST,
                               climate::CLIMATE_PRESET_ECO, climate::CLIMATE_PRESET_SLEEP}) {}

  void dump_config() override;

 protected:
  // Transmit current state as an IR frame.
  void transmit_state() override;
  // Decode an incoming frame (hand-held remote) and update HA state.
  bool on_receive(remote_base::RemoteReceiveData data) override;

  // Build the 5×32-bit frame for the current climate state.
  void build_frame_(uint32_t out[MHI_NBLOCKS]);

  // Last non-OFF mode, so a power-off frame keeps the previous mode field.
  climate::ClimateMode last_active_mode_{climate::CLIMATE_MODE_COOL};

  // Last transmitted data blocks (B1, B3) + timestamp, used to recognise and
  // ignore our own LED echo. B2/B4 are skipped since RX-side demodulator drift
  // can flip bits in the complement blocks.
  uint32_t last_tx_b1_{0};
  uint32_t last_tx_b3_{0};
  uint32_t last_transmit_ms_{0};
};

}  // namespace mhi_pjz
}  // namespace esphome
