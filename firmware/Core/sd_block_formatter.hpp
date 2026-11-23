#ifndef P05_CORE_SD_BLOCK_FORMATTER_HPP
#define P05_CORE_SD_BLOCK_FORMATTER_HPP

#include <array>
#include <cstdint>

#include "../HIL/trusted_types.hpp"

namespace p05 {

// Header FLAGS bitmask approved in Rule/PROTOCOL.md section 3.2 (PROPOSED-SD-01)
namespace sd_flags {
inline constexpr uint16_t kLodP = 1U << 0U;
inline constexpr uint16_t kLodN = 1U << 1U;
inline constexpr uint16_t kSoftFloat = 1U << 2U;
inline constexpr uint16_t kFs500Hz = 1U << 3U;
inline constexpr uint16_t kPingPongOverflowGap = 1U << 4U;
inline constexpr uint16_t kLowBattery = 1U << 5U;
}  // namespace sd_flags

using SdSectorBuffer = std::array<uint8_t, kSdBlockBytes>;

// Encodes a 512-byte RAW_ECG_BLOCK (PROPOSED-SD-01):
// 16 B Header ("ECG5", v1, type 0x01, flags, seq, ts_ms)
// + 2 B valid_sample_count (1..245) + 490 B int16_t[245] LE
// + 2 B reserved (0x0000) + 2 B CRC-16/CCITT-FALSE LE = 512 B.
// Fails with Malformed/SemanticRange if batch has 0 or >245 samples or out-of-range ADC codes.
Result<SdSectorBuffer> format_sd_raw_ecg_block(uint32_t block_seq,
                                               uint32_t timestamp_ms,
                                               uint16_t flags,
                                               const TrustedWearableBatch& batch) noexcept;

// Input snapshot for encoding a 512-byte RR_HRV_BLOCK (PROPOSED-SD-02, OPEN-19).
struct SdRrHrvBlockInput {
    uint32_t block_seq = 0U;
    uint32_t timestamp_ms = 0U;
    uint16_t flags = 0U;
    uint8_t valid_rr_count = 0U;
    std::array<TrustedRPeakRecord, kSdMaxRrRecords> peaks{};
    Result<TrustedHrvMetrics> hrv_res =
        Result<TrustedHrvMetrics>::fail(CanonicalState::SensorUnavailable, DetailCode::LeadsOff);
    Result<TrustedBatteryReading> battery_res =
        Result<TrustedBatteryReading>::fail(CanonicalState::SensorUnavailable, DetailCode::LowBattery);
    Result<TrustedIna219Reading> ina219_res =
        Result<TrustedIna219Reading>::fail(CanonicalState::CommsFault, DetailCode::I2cIna219Fault);
};

// Encodes a 512-byte RR_HRV_BLOCK (PROPOSED-SD-02):
// 16 B Header + 492 B Body (4 + 4 + 16 + 460 + 8 padding) + 4 B Trailer (CRC-16) = 512 B.
// Note: when hrv_res / battery_res / ina219_res are invalid, wire placeholders are explicitly
// flagged (hrv_valid = 0; vbat/bus = 0 with sensor fault flag) so R2 is preserved on readout.
Result<SdSectorBuffer> format_sd_rr_hrv_block(const SdRrHrvBlockInput& input) noexcept;

}  // namespace p05

#endif  // P05_CORE_SD_BLOCK_FORMATTER_HPP
