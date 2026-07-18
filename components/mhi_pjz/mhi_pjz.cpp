#include "mhi_pjz.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <cinttypes>
#include <cmath>

namespace esphome {
namespace mhi_pjz {

static const char *const TAG = "mhi_pjz.climate";

void MhiPjzClimate::dump_config() {
  ESP_LOGCONFIG(TAG, "Mitsubishi Heavy Industries PJZ IR Climate");
  this->dump_traits_(TAG);
}

// Customer code (protocol bits 0–13): 10110000000000 = 11264.
static const uint32_t MHI_CUSTOMER_CODE = 0b10110000000000;
// Static Block 5 (verified): 01000000101111110000000000000000.
static const uint32_t MHI_BLOCK5 = 0x40BF0000u;

// Full Block 3 values per fan stage (PROTOCOL.md, all 5 stages verified).
// Index by encoded fan value: bits0-2 as MSB-first 3-bit value.
//   auto=001=1, stage1=000=0, stage2=100=4, stage3=010=2, stage4=110=6
static uint32_t block3_for_fan(uint8_t fan_code) {
  switch (fan_code) {
    case 0:  // stage 1 (low)
      return 0x00802800u;
    case 4:  // stage 2
      return 0x80802800u;
    case 2:  // stage 3
      return 0x40802800u;
    case 6:  // stage 4 (high)
      return 0xC0802800u;
    case 1:  // auto
    default:
      return 0x20802800u;
  }
}

// Set protocol bit `pos` (0 = MSB) in a 32-bit block.
static inline void set_proto_bit(uint32_t &block, int pos) { block |= (1u << (31 - pos)); }
// Read protocol bit `pos` (0 = MSB).
static inline bool get_proto_bit(uint32_t block, int pos) { return (block >> (31 - pos)) & 1u; }

void MhiPjzClimate::build_frame_(uint32_t out[MHI_NBLOCKS]) {
  const bool power = this->mode != climate::CLIMATE_MODE_OFF;
  const climate::ClimateMode m = power ? this->mode : this->last_active_mode_;

  const bool boost = this->preset.has_value() && this->preset.value() == climate::CLIMATE_PRESET_BOOST;
  const bool eco = this->preset.has_value() && this->preset.value() == climate::CLIMATE_PRESET_ECO;
  const bool silent = this->preset.has_value() && this->preset.value() == climate::CLIMATE_PRESET_SLEEP;

  // ─── Block 1 ───────────────────────────────────────────────────────────────
  uint32_t b1 = 0;
  // Customer code 10110000000000 → bits 0,2,3.
  set_proto_bit(b1, 0);
  set_proto_bit(b1, 2);
  set_proto_bit(b1, 3);

  // Bit 14: swing-active flag (1 = auto/swing, 0 = fixed position).
  const bool swing_auto = this->swing_mode == climate::CLIMATE_SWING_VERTICAL;
  if (swing_auto)
    set_proto_bit(b1, 14);

  // Bits 16–19: temperature (LSB-first), value = °C − 16.
  // Hi-Power (BOOST) sends the 0000 underflow code → leave temp bits at 0.
  // Econo (ECO) forces 28 °C (matches the remote's behaviour).
  if (!boost) {
    int temp_c = eco ? 28 : static_cast<int>(lroundf(this->target_temperature));
    if (temp_c < static_cast<int>(MHI_TEMP_MIN))
      temp_c = static_cast<int>(MHI_TEMP_MIN);
    if (temp_c > static_cast<int>(MHI_TEMP_MAX))
      temp_c = static_cast<int>(MHI_TEMP_MAX);
    auto temp_val = static_cast<uint8_t>(temp_c - 16);
    for (int i = 0; i < 4; i++) {
      if ((temp_val >> i) & 1)
        set_proto_bit(b1, 16 + i);  // bit 16 = LSB
    }
  }

  // Bits 20–22: mode (MSB-first 3-bit pattern, see PROTOCOL.md).
  //   auto 000, heat 001, cool 010, dry 100, fan 110
  uint8_t mode_code = 0;
  switch (m) {
    case climate::CLIMATE_MODE_HEAT:
      mode_code = 0b001;
      break;
    case climate::CLIMATE_MODE_COOL:
      mode_code = 0b010;
      break;
    case climate::CLIMATE_MODE_DRY:
      mode_code = 0b100;
      break;
    case climate::CLIMATE_MODE_FAN_ONLY:
      mode_code = 0b110;
      break;
    case climate::CLIMATE_MODE_HEAT_COOL:
    case climate::CLIMATE_MODE_AUTO:
    default:
      mode_code = 0b000;
      break;
  }
  if ((mode_code >> 2) & 1)
    set_proto_bit(b1, 20);
  if ((mode_code >> 1) & 1)
    set_proto_bit(b1, 21);
  if (mode_code & 1)
    set_proto_bit(b1, 22);

  // Bit 23: power.
  if (power)
    set_proto_bit(b1, 23);

  // Bits 28–31: swing position. Auto = 1100; fixed (OFF) = pos1 = 0000.
  if (swing_auto) {
    set_proto_bit(b1, 28);
    set_proto_bit(b1, 29);
  }

  // ─── Block 3 ───────────────────────────────────────────────────────────────
  uint8_t fan_code = 1;  // auto
  if (this->fan_mode.has_value()) {
    switch (this->fan_mode.value()) {
      case climate::CLIMATE_FAN_LOW:
        fan_code = 0;
        break;
      case climate::CLIMATE_FAN_MEDIUM:
        fan_code = 4;
        break;
      case climate::CLIMATE_FAN_MIDDLE:
        fan_code = 2;
        break;
      case climate::CLIMATE_FAN_HIGH:
        fan_code = 6;
        break;
      case climate::CLIMATE_FAN_AUTO:
      default:
        fan_code = 1;
        break;
    }
  }
  uint32_t b3 = block3_for_fan(fan_code);
  if (boost)
    b3 |= (1u << 9);  // Block 3 protocol bit 22 = Hi-Power flag
  if (eco)
    b3 |= (1u << 8);  // Block 3 protocol bit 23 = Econo flag
  if (silent)
    b3 |= MHI_SILENT_FLAG;  // Block 3 protocol bit 15

  out[0] = b1;
  out[1] = ~b1;
  out[2] = b3;
  out[3] = ~b3;
  out[4] = MHI_BLOCK5;
}

void MhiPjzClimate::transmit_state() {
  if (this->mode != climate::CLIMATE_MODE_OFF)
    this->last_active_mode_ = this->mode;
  // Econo forces 28 °C — the physical remote's display jumps to 28 as well.
  // Setting it here (before ClimateIR publishes) keeps the HA setpoint in
  // sync with what the frame actually encodes.
  if (this->preset.has_value() && this->preset.value() == climate::CLIMATE_PRESET_ECO)
    this->target_temperature = 28.0f;

  uint32_t blocks[MHI_NBLOCKS];
  this->build_frame_(blocks);

  this->last_tx_b1_ = blocks[0];
  this->last_tx_b3_ = blocks[2];
  this->last_transmit_ms_ = App.get_loop_component_start_time();

  auto transmit = this->transmitter_->transmit();
  auto *data = transmit.get_data();
  data->set_carrier_frequency(MHI_CARRIER);
  data->reserve(2 + MHI_NBITS * 2 + 3);

  data->mark(MHI_HEADER_MARK);
  data->space(MHI_HEADER_SPACE);
  for (int blk = 0; blk < MHI_NBLOCKS; blk++) {
    for (int i = 0; i < 32; i++) {
      data->mark(MHI_BIT_MARK);
      bool bit = (blocks[blk] >> (31 - i)) & 1u;
      data->space(bit ? MHI_ONE_SPACE : MHI_ZERO_SPACE);
    }
  }
  data->mark(MHI_END_PULSE);
  data->space(MHI_END_GAP);
  // Trailing mark: the MHI unit treats the end-gap as a frame-internal long
  // space and only accepts the frame once it is terminated by a final mark.
  // Without this pulse the frame ends on a space and is discarded. Verified on
  // the device (captures A/B/D with the mark work, capture C without it does
  // not); cf. ToniA MitsubishiHeavyFDTC: mark, space(gap), mark.
  data->mark(MHI_END_PULSE);

  ESP_LOGD(TAG, "TX frame: %08" PRIX32 " %08" PRIX32 " %08" PRIX32 " %08" PRIX32 " %08" PRIX32, blocks[0], blocks[1],
           blocks[2], blocks[3], blocks[4]);
  transmit.perform();
}

bool MhiPjzClimate::on_receive(remote_base::RemoteReceiveData data) {
  // Header.
  if (!data.expect_item(MHI_HEADER_MARK, MHI_HEADER_SPACE))
    return false;

  uint32_t blocks[MHI_NBLOCKS] = {0};
  for (int blk = 0; blk < MHI_NBLOCKS; blk++) {
    for (int i = 0; i < 32; i++) {
      // Bit mark: accept ANY mark length instead of expect_mark(MHI_BIT_MARK).
      // The IR receiver's AGC droops the mark from ~500 us down to well under
      // 150 us across this long (160-bit) frame, so a fixed-length check drops
      // most real frames (silently, since expect_mark logs nothing). The bit
      // value lives entirely in the following space, so the mark length is
      // irrelevant — we only require that a mark is present and a space follows.
      // Need both the mark (index) and its space (index+1) in the buffer.
      if (!data.is_valid(1) || data.peek() <= 0)
        return false;  // expected a mark here (any length)
      data.advance();
      int32_t space = data.peek();  // current item: negative = space
      data.advance();
      if (space >= 0)
        return false;  // expected a space here
      if (static_cast<uint32_t>(-space) > MHI_SPACE_THRESHOLD)
        blocks[blk] |= (1u << (31 - i));
    }
  }

  // Ignore our own LED echo: a frame matching our last transmit (B1+B3) that
  // arrives within the suppression window. Avoids redundant publishes and
  // state flicker from delayed self-reception.
  if (this->last_transmit_ms_ != 0 &&
      (App.get_loop_component_start_time() - this->last_transmit_ms_) < MHI_SELF_RX_SUPPRESS_MS &&
      blocks[0] == this->last_tx_b1_ && blocks[2] == this->last_tx_b3_) {
    ESP_LOGV(TAG, "RX ignored: own transmit echo");
    return false;
  }

  // Hard check: customer code must match (reject foreign protocols).
  if ((blocks[0] >> 18) != MHI_CUSTOMER_CODE) {
    ESP_LOGV(TAG, "RX rejected: customer code mismatch (%08" PRIX32 ")", blocks[0]);
    return false;
  }

  const uint32_t b1 = blocks[0];
  const uint32_t b3 = blocks[2];
  ESP_LOGD(TAG, "RX frame: %08" PRIX32 " %08" PRIX32 " %08" PRIX32 " %08" PRIX32 " %08" PRIX32, blocks[0], blocks[1],
           blocks[2], blocks[3], blocks[4]);

  // Power (bit 23) and mode (bits 20–22).
  const bool power = get_proto_bit(b1, 23);
  uint8_t mode_code = (get_proto_bit(b1, 20) << 2) | (get_proto_bit(b1, 21) << 1) | get_proto_bit(b1, 22);

  if (!power) {
    this->mode = climate::CLIMATE_MODE_OFF;
  } else {
    switch (mode_code) {
      case 0b001:
        this->mode = climate::CLIMATE_MODE_HEAT;
        break;
      case 0b010:
        this->mode = climate::CLIMATE_MODE_COOL;
        break;
      case 0b100:
        this->mode = climate::CLIMATE_MODE_DRY;
        break;
      case 0b110:
        this->mode = climate::CLIMATE_MODE_FAN_ONLY;
        break;
      case 0b000:
      default:
        this->mode = climate::CLIMATE_MODE_AUTO;
        break;
    }
    this->last_active_mode_ = this->mode;
  }

  // Presets from Block 3 flag bits.
  const bool rx_boost = get_proto_bit(b3, 22);
  const bool rx_eco = get_proto_bit(b3, 23);
  const bool rx_silent = (b3 & MHI_SILENT_FLAG) != 0;  // Block 3 protocol bit 15
  if (rx_boost)
    this->preset = climate::CLIMATE_PRESET_BOOST;
  else if (rx_eco)
    this->preset = climate::CLIMATE_PRESET_ECO;
  else if (rx_silent)
    this->preset = climate::CLIMATE_PRESET_SLEEP;
  else
    this->preset = climate::CLIMATE_PRESET_NONE;

  // Temperature (bits 16–19, LSB-first). Hi-Power sends the 0000 underflow code
  // (decodes to 16 °C, below range) — keep the previous setpoint instead.
  if (!rx_boost) {
    uint8_t temp_val = 0;
    for (int i = 0; i < 4; i++)
      temp_val |= (get_proto_bit(b1, 16 + i) << i);
    this->target_temperature = static_cast<float>(temp_val + 16);
  }

  // Swing (bit 14).
  this->swing_mode =
      get_proto_bit(b1, 14) ? climate::CLIMATE_SWING_VERTICAL : climate::CLIMATE_SWING_OFF;

  // Fan (block 3, bits 0–2).
  uint8_t fan_code = (get_proto_bit(b3, 0) << 2) | (get_proto_bit(b3, 1) << 1) | get_proto_bit(b3, 2);
  switch (fan_code) {
    case 0:
      this->fan_mode = climate::CLIMATE_FAN_LOW;
      break;
    case 4:
      this->fan_mode = climate::CLIMATE_FAN_MEDIUM;
      break;
    case 2:
      this->fan_mode = climate::CLIMATE_FAN_MIDDLE;
      break;
    case 6:
      this->fan_mode = climate::CLIMATE_FAN_HIGH;
      break;
    case 1:
    default:
      this->fan_mode = climate::CLIMATE_FAN_AUTO;
      break;
  }

  this->publish_state();
  return true;
}

}  // namespace mhi_pjz
}  // namespace esphome
