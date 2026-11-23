#include "sd_block_formatter.hpp"

namespace p05 {

namespace {

inline constexpr uint8_t kSdSync0 = 0x45U;  // 'E'
inline constexpr uint8_t kSdSync1 = 0x43U;  // 'C'
inline constexpr uint8_t kSdSync2 = 0x47U;  // 'G'
inline constexpr uint8_t kSdSync3 = 0x35U;  // '5'
inline constexpr uint8_t kSdVersion = 0x01U;

void write_sd_header(SdSectorBuffer& buf,
                     SdBlockType type,
                     uint16_t flags,
                     uint32_t block_seq,
                     uint32_t timestamp_ms) noexcept {
    buf[0] = kSdSync0;
    buf[1] = kSdSync1;
    buf[2] = kSdSync2;
    buf[3] = kSdSync3;
    buf[4] = kSdVersion;
    buf[5] = static_cast<uint8_t>(type);
    write_u16_le(&buf[6], flags);
    write_u32_le(&buf[8], block_seq);
    write_u32_le(&buf[12], timestamp_ms);
}

void write_sd_trailer(SdSectorBuffer& buf) noexcept {
    write_u16_le(&buf[508], 0x0000U);
    const uint16_t crc = crc16_ccitt_false(buf.data(), 510U);
    write_u16_le(&buf[510], crc);
}

}  // namespace

Result<SdSectorBuffer> format_sd_raw_ecg_block(uint32_t block_seq,
                                               uint32_t timestamp_ms,
                                               uint16_t flags,
                                               const TrustedWearableBatch& batch) noexcept {
    if (batch.sample_count == 0U || batch.sample_count > kSdMaxRawSamples) {
        return Result<SdSectorBuffer>::fail(CanonicalState::Malformed, DetailCode::SemanticRange);
    }

    SdSectorBuffer buf{};
    buf.fill(0U);
    write_sd_header(buf, SdBlockType::RawEcg, flags, block_seq, timestamp_ms);
    write_u16_le(&buf[16], batch.sample_count);

    for (std::size_t i = 0U; i < static_cast<std::size_t>(batch.sample_count); ++i) {
        const int16_t s = batch.samples[i];
        if (s < static_cast<int16_t>(kAdcRawMin) || s > static_cast<int16_t>(kAdcRawMax)) {
            return Result<SdSectorBuffer>::fail(CanonicalState::Malformed,
                                                DetailCode::SemanticRange);
        }
        write_i16_le(&buf[18U + (2U * i)], s);
    }

    write_sd_trailer(buf);
    return Result<SdSectorBuffer>::ok(buf);
}

Result<SdSectorBuffer> format_sd_rr_hrv_block(const SdRrHrvBlockInput& input) noexcept {
    if (input.valid_rr_count > kSdMaxRrRecords) {
        return Result<SdSectorBuffer>::fail(CanonicalState::Malformed, DetailCode::SemanticRange);
    }

    SdSectorBuffer buf{};
    buf.fill(0U);
    write_sd_header(buf, SdBlockType::RrHrv, input.flags, input.block_seq, input.timestamp_ms);

    buf[16] = input.valid_rr_count;
    buf[17] = input.hrv_res.is_ok() ? 1U : 0U;

    const uint16_t vbat_mv = input.battery_res.is_ok() ? input.battery_res.value().vbat_mv : 0U;
    write_u16_le(&buf[18], vbat_mv);

    int16_t cur_div10 = 0;
    uint16_t bus_mv = 0U;
    if (input.ina219_res.is_ok()) {
        const int32_t scaled = input.ina219_res.value().current_ua / 10;
        if (scaled > 32767) {
            cur_div10 = 32767;
        } else if (scaled < -32768) {
            cur_div10 = -32768;
        } else {
            cur_div10 = static_cast<int16_t>(scaled);
        }
        bus_mv = input.ina219_res.value().bus_mv;
    }
    write_i16_le(&buf[20], cur_div10);
    write_u16_le(&buf[22], bus_mv);

    if (input.hrv_res.is_ok()) {
        const TrustedHrvMetrics& m = input.hrv_res.value();
        write_u32_le(&buf[24], m.mean_rr_us);
        write_u32_le(&buf[28], m.sdnn_us);
        write_u32_le(&buf[32], m.rmssd_us);
        write_u16_le(&buf[36], m.hr_bpm_x100);
        write_u16_le(&buf[38], m.pnn50_permille);
    }

    for (std::size_t i = 0U; i < static_cast<std::size_t>(input.valid_rr_count); ++i) {
        const std::size_t off = 40U + (10U * i);
        write_u32_le(&buf[off], input.peaks[i].peak_sample_idx);
        write_i16_le(&buf[off + 4U], input.peaks[i].sub_sample_offset_q15);
        write_u32_le(&buf[off + 6U], input.peaks[i].rr_interval_us);
    }

    // Bytes 500..507 already zeroed as reserved_padding[8]
    write_sd_trailer(buf);
    return Result<SdSectorBuffer>::ok(buf);
}

}  // namespace p05
