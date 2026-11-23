#include <array>
#include <cstdint>
#include <cstdio>

#include "../Core/sensor_storage_boundary.hpp"
#include "../HIL/boundary_parser.hpp"
#include "../HIL/trusted_types.hpp"

namespace {

int g_tests_run = 0;
int g_tests_failed = 0;

#define EXPECT_TRUE(cond)                                                                      \
    do {                                                                                       \
        ++g_tests_run;                                                                         \
        if (!(cond)) {                                                                         \
            ++g_tests_failed;                                                                  \
            std::printf("FAIL [%s:%d]: %s\n", __FILE__, __LINE__, #cond);                      \
        }                                                                                      \
    } while (false)

#define EXPECT_EQ(a, b)                                                                        \
    do {                                                                                       \
        ++g_tests_run;                                                                         \
        if (!((a) == (b))) {                                                                   \
            ++g_tests_failed;                                                                  \
            std::printf("FAIL [%s:%d]: %s != %s\n", __FILE__, __LINE__, #a, #b);               \
        }                                                                                      \
    } while (false)

struct BuiltFrame {
    std::array<uint8_t, p05::kHilMaxFrameBytes> bytes{};
    std::size_t length = 0U;

    p05::ConstByteView view() const noexcept { return p05::ConstByteView{bytes.data(), length}; }
};

BuiltFrame build_hil_config_frame(uint16_t seq,
                                  uint16_t fs_hz,
                                  uint8_t math_mode,
                                  uint8_t interp_en,
                                  uint8_t reset_state) noexcept {
    BuiltFrame f{};
    f.bytes[0] = p05::kHilSync0;
    f.bytes[1] = p05::kHilSync1;
    f.bytes[2] = p05::kHilVersion;
    f.bytes[3] = static_cast<uint8_t>(p05::HilMsgType::ConfigReq);
    p05::write_u16_le(&f.bytes[4], seq);
    p05::write_u16_le(&f.bytes[6], 5U);
    p05::write_u16_le(&f.bytes[8], fs_hz);
    f.bytes[10] = math_mode;
    f.bytes[11] = interp_en;
    f.bytes[12] = reset_state;
    const uint16_t crc = p05::crc16_ccitt_false(f.bytes.data(), 13U);
    p05::write_u16_le(&f.bytes[13], crc);
    f.length = 15U;
    return f;
}

BuiltFrame build_hil_samples_frame(uint16_t seq,
                                   uint32_t first_idx,
                                   const int16_t* samples,
                                   uint8_t count) noexcept {
    BuiltFrame f{};
    const uint16_t payload_len = static_cast<uint16_t>(5U + (2U * static_cast<uint16_t>(count)));
    f.bytes[0] = p05::kHilSync0;
    f.bytes[1] = p05::kHilSync1;
    f.bytes[2] = p05::kHilVersion;
    f.bytes[3] = static_cast<uint8_t>(p05::HilMsgType::SamplesIn);
    p05::write_u16_le(&f.bytes[4], seq);
    p05::write_u16_le(&f.bytes[6], payload_len);
    p05::write_u32_le(&f.bytes[8], first_idx);
    f.bytes[12] = count;
    for (std::size_t i = 0U; i < static_cast<std::size_t>(count); ++i) {
        p05::write_i16_le(&f.bytes[13U + (2U * i)], samples[i]);
    }
    const std::size_t crc_off = p05::kHilHeaderBytes + static_cast<std::size_t>(payload_len);
    const uint16_t crc = p05::crc16_ccitt_false(f.bytes.data(), crc_off);
    p05::write_u16_le(&f.bytes[crc_off], crc);
    f.length = crc_off + p05::kHilCrcBytes;
    return f;
}

std::array<uint8_t, p05::kSdBlockBytes> build_sd_raw_block(uint32_t seq,
                                                           uint32_t ts_ms,
                                                           uint16_t flags,
                                                           uint16_t valid_count,
                                                           int16_t sample_fill) noexcept {
    std::array<uint8_t, p05::kSdBlockBytes> blk{};
    blk[0] = 0x45U;
    blk[1] = 0x43U;
    blk[2] = 0x47U;
    blk[3] = 0x35U;
    blk[4] = 0x01U;
    blk[5] = static_cast<uint8_t>(p05::SdBlockType::RawEcg);
    p05::write_u16_le(&blk[6], flags);
    p05::write_u32_le(&blk[8], seq);
    p05::write_u32_le(&blk[12], ts_ms);
    p05::write_u16_le(&blk[16], valid_count);
    const std::size_t fill_n =
        (valid_count <= p05::kSdMaxRawSamples) ? valid_count : p05::kSdMaxRawSamples;
    for (std::size_t i = 0U; i < fill_n; ++i) {
        p05::write_i16_le(&blk[18U + (2U * i)], sample_fill);
    }
    p05::write_u16_le(&blk[508], 0x0000U);
    const uint16_t crc = p05::crc16_ccitt_false(blk.data(), 510U);
    p05::write_u16_le(&blk[510], crc);
    return blk;
}

std::array<uint8_t, p05::kSdBlockBytes> build_sd_rr_block(uint32_t seq,
                                                          uint32_t ts_ms,
                                                          uint8_t rr_count,
                                                          uint8_t hrv_valid) noexcept {
    std::array<uint8_t, p05::kSdBlockBytes> blk{};
    blk[0] = 0x45U;
    blk[1] = 0x43U;
    blk[2] = 0x47U;
    blk[3] = 0x35U;
    blk[4] = 0x01U;
    blk[5] = static_cast<uint8_t>(p05::SdBlockType::RrHrv);
    p05::write_u16_le(&blk[6], 0x0000U);
    p05::write_u32_le(&blk[8], seq);
    p05::write_u32_le(&blk[12], ts_ms);
    blk[16] = rr_count;
    blk[17] = hrv_valid;
    p05::write_u16_le(&blk[18], 3950U);     // vbat_mv
    p05::write_i16_le(&blk[20], 510);       // 5.10 mA in 10 uA units
    p05::write_u16_le(&blk[22], 3300U);     // bus_mv
    p05::write_u32_le(&blk[24], 833333U);   // mean_rr_us (~72 bpm)
    p05::write_u32_le(&blk[28], 42000U);    // sdnn_us
    p05::write_u32_le(&blk[32], 31500U);    // rmssd_us
    p05::write_u16_le(&blk[36], 7200U);     // hr_bpm_x100
    p05::write_u16_le(&blk[38], 125U);      // pnn50_permille
    const std::size_t fill_n =
        (rr_count <= p05::kSdMaxRrRecords) ? rr_count : p05::kSdMaxRrRecords;
    for (std::size_t i = 0U; i < fill_n; ++i) {
        const std::size_t off = 40U + (10U * i);
        p05::write_u32_le(&blk[off], static_cast<uint32_t>(100U + (200U * i)));
        p05::write_i16_le(&blk[off + 4U], -128);
        p05::write_u32_le(&blk[off + 6U], 833000U);
    }
    p05::write_u16_le(&blk[508], 0x0000U);
    const uint16_t crc = p05::crc16_ccitt_false(blk.data(), 510U);
    p05::write_u16_le(&blk[510], crc);
    return blk;
}

void test_01_crc16_and_result_invariants() {
    // OPEN-13 standard check vector: "123456789" -> 0x29B1
    const std::array<uint8_t, 9> vec = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    EXPECT_EQ(p05::crc16_ccitt_false(vec.data(), vec.size()), 0x29B1U);

    // Result<T> cannot be inconsistent (RULES section 4)
    const auto r_ok = p05::Result<int>::ok(42);
    EXPECT_TRUE(r_ok.is_ok());
    EXPECT_TRUE(!r_ok.is_err());
    EXPECT_EQ(r_ok.canonical_state(), p05::CanonicalState::Valid);
    EXPECT_EQ(r_ok.detail_code(), p05::DetailCode::Ok);
    EXPECT_EQ(r_ok.value(), 42);

    const auto r_err =
        p05::Result<int>::fail(p05::CanonicalState::Malformed, p05::DetailCode::BadCrc);
    EXPECT_TRUE(r_err.is_err());
    EXPECT_TRUE(!r_err.optional_value().has_value());
    EXPECT_EQ(r_err.canonical_state(), p05::CanonicalState::Malformed);
    EXPECT_EQ(r_err.detail_code(), p05::DetailCode::BadCrc);

    // Defensive check: fail(Valid, ...) cannot create a Valid-without-value contradiction
    const auto r_def =
        p05::Result<int>::fail(p05::CanonicalState::Valid, p05::DetailCode::Ok);
    EXPECT_TRUE(r_def.is_err());
    EXPECT_EQ(r_def.canonical_state(), p05::CanonicalState::ProtectionStatusUnknown);
}

void test_02_stateless_hil_parser_valid_and_boundary_rejections() {
    // Valid CONFIG frame (500 Hz, SoftFloat, interp=0, reset=1)
    const BuiltFrame cfg = build_hil_config_frame(0x1234U, 500U, 1U, 0U, 1U);
    const auto r_cfg = p05::parse_hil_frame_stateless(cfg.view(), 250U);
    EXPECT_TRUE(r_cfg.is_ok());
    EXPECT_EQ(r_cfg.value().msg_type, p05::HilMsgType::ConfigReq);
    EXPECT_EQ(r_cfg.value().seq, 0x1234U);
    EXPECT_EQ(r_cfg.value().config.fs_hz, 500U);
    EXPECT_EQ(r_cfg.value().config.math_mode, p05::MathMode::SoftFloat);
    EXPECT_EQ(r_cfg.value().config.interp_enable, false);
    EXPECT_EQ(r_cfg.value().config.reset_state, true);

    // Valid SAMPLES frame at max batch size (64 samples) with boundary ADC values 0 and 4095
    std::array<int16_t, p05::kMaxBatchSamples> max_samples{};
    for (std::size_t i = 0U; i < max_samples.size(); ++i) {
        max_samples[i] = static_cast<int16_t>((i == 0U) ? 0 : ((i == 63U) ? 4095 : 2048));
    }
    const BuiltFrame smp =
        build_hil_samples_frame(0x0005U, 1000U, max_samples.data(), p05::kMaxBatchSamples);
    EXPECT_EQ(smp.length, p05::kHilMaxFrameBytes);
    const auto r_smp = p05::parse_hil_frame_stateless(smp.view(), 250U);
    EXPECT_TRUE(r_smp.is_ok());
    EXPECT_EQ(r_smp.value().batch.sample_count, 64U);
    EXPECT_EQ(r_smp.value().batch.first_sample_idx, 1000U);
    EXPECT_EQ(r_smp.value().batch.samples[0], 0);
    EXPECT_EQ(r_smp.value().batch.samples[63], 4095);

    // 1. Null / truncated / wrong length rejected FIRST
    const auto r_null = p05::parse_hil_frame_stateless(p05::ConstByteView{nullptr, 15U}, 250U);
    EXPECT_EQ(r_null.detail_code(), p05::DetailCode::BadLength);

    const auto r_trunc =
        p05::parse_hil_frame_stateless(p05::ConstByteView{cfg.bytes.data(), cfg.length - 1U}, 250U);
    EXPECT_EQ(r_trunc.canonical_state(), p05::CanonicalState::Malformed);
    EXPECT_EQ(r_trunc.detail_code(), p05::DetailCode::BadLength);

    // Check strict order: bad length + bad sync must report BadLength first
    BuiltFrame bad_len_and_sync = cfg;
    bad_len_and_sync.bytes[0] = 0x00U;
    const auto r_order1 = p05::parse_hil_frame_stateless(
        p05::ConstByteView{bad_len_and_sync.bytes.data(), 9U}, 250U);
    EXPECT_EQ(r_order1.detail_code(), p05::DetailCode::BadLength);

    // 2. Bad sync
    BuiltFrame bad_sync = cfg;
    bad_sync.bytes[1] = 0x5BU;
    const auto r_sync = p05::parse_hil_frame_stateless(bad_sync.view(), 250U);
    EXPECT_EQ(r_sync.canonical_state(), p05::CanonicalState::Malformed);
    EXPECT_EQ(r_sync.detail_code(), p05::DetailCode::BadSync);

    // 3. Bad version
    BuiltFrame bad_ver = cfg;
    bad_ver.bytes[2] = 0x02U;
    const auto r_ver = p05::parse_hil_frame_stateless(bad_ver.view(), 250U);
    EXPECT_EQ(r_ver.detail_code(), p05::DetailCode::BadVersion);

    // 4. R1: Bad CRC MUST be rejected BEFORE semantic decode (even when fs_hz is invalid 999)
    BuiltFrame bad_crc = build_hil_config_frame(1U, 999U, 0U, 1U, 1U);
    bad_crc.bytes[bad_crc.length - 1U] ^= 0x01U;  // corrupt 1 bit in CRC
    const auto r_crc = p05::parse_hil_frame_stateless(bad_crc.view(), 250U);
    EXPECT_EQ(r_crc.canonical_state(), p05::CanonicalState::Malformed);
    EXPECT_EQ(r_crc.detail_code(), p05::DetailCode::BadCrc);
    EXPECT_TRUE(!r_crc.optional_value().has_value());

    // 5. Valid CRC but invalid semantic values
    const BuiltFrame bad_fs = build_hil_config_frame(2U, 360U, 0U, 1U, 1U);
    EXPECT_EQ(p05::parse_hil_frame_stateless(bad_fs.view(), 250U).detail_code(),
              p05::DetailCode::SemanticRange);

    const std::array<int16_t, 2> bad_adc_neg = {2048, -1};
    const BuiltFrame bad_s1 = build_hil_samples_frame(3U, 0U, bad_adc_neg.data(), 2U);
    EXPECT_EQ(p05::parse_hil_frame_stateless(bad_s1.view(), 250U).detail_code(),
              p05::DetailCode::SemanticRange);

    const std::array<int16_t, 2> bad_adc_hi = {2048, 4096};
    const BuiltFrame bad_s2 = build_hil_samples_frame(4U, 0U, bad_adc_hi.data(), 2U);
    EXPECT_EQ(p05::parse_hil_frame_stateless(bad_s2.view(), 250U).detail_code(),
              p05::DetailCode::SemanticRange);
}

void test_03_stateful_hil_sequence_gap_duplicate_replay_and_stale() {
    p05::HilBoundaryValidator validator;
    const p05::HilSafetyInputs safe_hil{true, true};
    const std::array<int16_t, 4> s = {2000, 2048, 2100, 2050};

    // Samples before HIL_CONFIG_REQ are rejected (OPEN-2)
    const BuiltFrame early_smp = build_hil_samples_frame(1U, 0U, s.data(), 4U);
    const auto r_early = validator.validate_incoming_frame(early_smp.view(), 100U, safe_hil);
    EXPECT_TRUE(r_early.is_err());
    EXPECT_EQ(r_early.canonical_state(), p05::CanonicalState::Malformed);
    EXPECT_TRUE(!validator.hil_session_active());

    // Start session at seq = 65535 to test 16-bit sequence wraparound too
    const BuiltFrame cfg = build_hil_config_frame(65535U, 250U, 0U, 1U, 1U);
    const auto r0 = validator.validate_incoming_frame(cfg.view(), 1000U, safe_hil);
    EXPECT_TRUE(r0.is_ok());
    EXPECT_TRUE(validator.hil_session_active());

    // Normal next seq = 0 (wraparound from 65535)
    const BuiltFrame smp0 = build_hil_samples_frame(0U, 0U, s.data(), 4U);
    const auto r1 = validator.validate_incoming_frame(smp0.view(), 1020U, safe_hil);
    EXPECT_TRUE(r1.is_ok());
    EXPECT_EQ(r1.detail_code(), p05::DetailCode::Ok);

    // Duplicate (seq == 0) -> Valid / DuplicateIgnored (OPEN-15 / PROPOSED-PROTO-04)
    const auto r_dup = validator.validate_incoming_frame(smp0.view(), 1030U, safe_hil);
    EXPECT_TRUE(r_dup.is_ok());
    EXPECT_EQ(r_dup.canonical_state(), p05::CanonicalState::Valid);
    EXPECT_EQ(r_dup.detail_code(), p05::DetailCode::DuplicateIgnored);

    // Replay (seq == 65535 < 0 in signed 16-bit delta) -> Malformed / SeqReplay
    const BuiltFrame smp_old = build_hil_samples_frame(65535U, 0U, s.data(), 4U);
    const auto r_rep = validator.validate_incoming_frame(smp_old.view(), 1040U, safe_hil);
    EXPECT_TRUE(r_rep.is_err());
    EXPECT_EQ(r_rep.canonical_state(), p05::CanonicalState::Malformed);
    EXPECT_EQ(r_rep.detail_code(), p05::DetailCode::SeqReplay);

    // Sequence gap (seq == 2, skipped seq 1) -> Stale / SeqGap (never fills zero samples!)
    const BuiltFrame smp_gap = build_hil_samples_frame(2U, 8U, s.data(), 4U);
    const auto r_gap = validator.validate_incoming_frame(smp_gap.view(), 1060U, safe_hil);
    EXPECT_TRUE(r_gap.is_err());
    EXPECT_EQ(r_gap.canonical_state(), p05::CanonicalState::Stale);
    EXPECT_EQ(r_gap.detail_code(), p05::DetailCode::SeqGap);
    EXPECT_TRUE(!r_gap.optional_value().has_value());

    // Sample index gap with contiguous seq (seq == 3, expected idx 12, got 20) -> Stale / SeqGap
    const BuiltFrame smp_idx_gap = build_hil_samples_frame(3U, 20U, s.data(), 4U);
    const auto r_igap = validator.validate_incoming_frame(smp_idx_gap.view(), 1080U, safe_hil);
    EXPECT_EQ(r_igap.canonical_state(), p05::CanonicalState::Stale);
    EXPECT_EQ(r_igap.detail_code(), p05::DetailCode::SeqGap);

    // OPEN-4: 3 consecutive malformed frames abort active HIL session
    BuiltFrame bad_crc = build_hil_samples_frame(4U, 24U, s.data(), 4U);
    bad_crc.bytes[bad_crc.length - 1U] ^= 0xFFU;
    EXPECT_EQ(validator.validate_incoming_frame(bad_crc.view(), 1100U, safe_hil).detail_code(),
              p05::DetailCode::BadCrc);
    EXPECT_TRUE(validator.hil_session_active());
    EXPECT_EQ(validator.validate_incoming_frame(bad_crc.view(), 1110U, safe_hil).detail_code(),
              p05::DetailCode::BadCrc);
    EXPECT_TRUE(validator.hil_session_active());
    EXPECT_EQ(validator.validate_incoming_frame(bad_crc.view(), 1120U, safe_hil).detail_code(),
              p05::DetailCode::BadCrc);
    EXPECT_EQ(validator.consecutive_malformed_count(), 3U);
    EXPECT_TRUE(!validator.hil_session_active());

    // OPEN-5: UART stale timeout (> 2000 ms), including uint32 wraparound
    const BuiltFrame cfg2 = build_hil_config_frame(10U, 250U, 0U, 1U, 1U);
    EXPECT_TRUE(validator.validate_incoming_frame(cfg2.view(), 0xFFFFFF00U, safe_hil).is_ok());
    const BuiltFrame smp_late = build_hil_samples_frame(11U, 0U, s.data(), 4U);
    // Elapsed = (0x000006D1 - 0xFFFFFF00) = 2001 ms > 2000 ms
    const auto r_stale = validator.validate_incoming_frame(smp_late.view(), 0x000006D1U, safe_hil);
    EXPECT_EQ(r_stale.canonical_state(), p05::CanonicalState::Stale);
    EXPECT_EQ(r_stale.detail_code(), p05::DetailCode::StaleTimeout);
    EXPECT_TRUE(!validator.hil_session_active());
}

void test_04_r3_comms_failure_cannot_clear_latched_protection_trip() {
    p05::HilBoundaryValidator validator;
    const BuiltFrame cfg = build_hil_config_frame(1U, 250U, 0U, 1U, 1U);

    // Electrodes attached while HIL_CONFIG_REQ arrives -> latches mode conflict lockout (OPEN-2 / R3)
    const p05::HilSafetyInputs attached{true, false};
    const auto r_conflict = validator.validate_incoming_frame(cfg.view(), 500U, attached);
    EXPECT_TRUE(r_conflict.is_err());
    EXPECT_EQ(r_conflict.canonical_state(), p05::CanonicalState::ProtectionStatusUnknown);
    EXPECT_EQ(r_conflict.detail_code(), p05::DetailCode::ModeConflictLockout);
    EXPECT_TRUE(validator.lockout_latched());

    // Comms failure MUST NOT clear the latched trip (RULES section 6)
    const auto comms_st = validator.on_comms_fault();
    EXPECT_EQ(comms_st.canonical, p05::CanonicalState::CommsFault);
    EXPECT_TRUE(validator.lockout_latched());

    // Timeout check also MUST NOT clear the latched trip
    const auto to_st = validator.check_timeout(5000U);
    EXPECT_EQ(to_st.canonical, p05::CanonicalState::ProtectionStatusUnknown);
    EXPECT_TRUE(validator.lockout_latched());

    // Even when electrodes detach, incoming frame stays blocked until explicit hardware-verified clear
    const p05::HilSafetyInputs detached{true, true};
    const auto r_still_latched = validator.validate_incoming_frame(cfg.view(), 6000U, detached);
    EXPECT_EQ(r_still_latched.canonical_state(), p05::CanonicalState::ProtectionStatusUnknown);
    EXPECT_TRUE(validator.lockout_latched());

    // Clearing while unsafe fails
    EXPECT_TRUE(!validator.try_clear_lockout(p05::HilSafetyInputs{false, true}));
    EXPECT_TRUE(!validator.try_clear_lockout(attached));
    EXPECT_TRUE(validator.lockout_latched());

    // Clearing ONLY succeeds when hardware reports protection known AND electrodes detached
    EXPECT_TRUE(validator.try_clear_lockout(detached));
    EXPECT_TRUE(!validator.lockout_latched());
    EXPECT_TRUE(validator.validate_incoming_frame(cfg.view(), 6100U, detached).is_ok());
}

void test_05_sensor_boundary_leads_off_battery_ina219_and_sd_blocks() {
    p05::SensorBoundaryMonitor mon;
    std::array<int16_t, p05::kSdMaxRawSamples> ecg{};
    ecg.fill(2048);

    p05::WearableBatchInputs in{};
    in.raw_samples = ecg.data();
    in.sample_count = p05::kSdMaxRawSamples;
    in.fs_hz = 250U;
    in.uptime_ms = 1000U;
    in.afe_enabled = false;

    // AFE shutdown -> SensorUnavailable / AfeShutdown (never zero samples!)
    const auto r_off = mon.validate_wearable_batch(in);
    EXPECT_EQ(r_off.canonical_state(), p05::CanonicalState::SensorUnavailable);
    EXPECT_EQ(r_off.detail_code(), p05::DetailCode::AfeShutdown);
    EXPECT_TRUE(!r_off.optional_value().has_value());

    // Valid batch at 1000 ms
    in.afe_enabled = true;
    EXPECT_TRUE(mon.validate_wearable_batch(in).is_ok());

    // Leads-Off glitch shorter than 50 ms debounce is ignored (OPEN-6)
    in.lod_p_high = true;
    in.uptime_ms = 1010U;
    EXPECT_TRUE(mon.validate_wearable_batch(in).is_ok());
    in.uptime_ms = 1049U;  // 39 ms < 50 ms
    EXPECT_TRUE(mon.validate_wearable_batch(in).is_ok());

    // At >= 50 ms, debounced Leads-Off activates -> SensorUnavailable / LeadsOff
    in.uptime_ms = 1060U;  // 50 ms since 1010 ms
    const auto r_lod = mon.validate_wearable_batch(in);
    EXPECT_EQ(r_lod.canonical_state(), p05::CanonicalState::SensorUnavailable);
    EXPECT_EQ(r_lod.detail_code(), p05::DetailCode::LeadsOff);
    EXPECT_TRUE(mon.leads_off_debounced());

    // Continuous 60 s Leads-Off triggers Stop Mode request (OPEN-6d)
    in.uptime_ms = 61060U;
    EXPECT_EQ(mon.validate_wearable_batch(in).detail_code(), p05::DetailCode::LeadsOff);
    EXPECT_TRUE(mon.stop_mode_requested());

    // Re-attach electrodes -> 50 ms debounce -> sets dsp_reset_pending() & no false stale timeout!
    in.lod_p_high = false;
    in.uptime_ms = 61100U;
    EXPECT_EQ(mon.validate_wearable_batch(in).detail_code(), p05::DetailCode::LeadsOff);
    in.uptime_ms = 61150U;
    const auto r_reattach = mon.validate_wearable_batch(in);
    EXPECT_TRUE(r_reattach.is_ok());
    EXPECT_TRUE(mon.consume_dsp_reset_pending());
    EXPECT_TRUE(!mon.consume_dsp_reset_pending());

    // ADC staleness @250 Hz: 1960 ms OK, 1961 ms -> Stale (OPEN-5)
    in.uptime_ms = 61150U + 1960U;
    EXPECT_TRUE(mon.validate_wearable_batch(in).is_ok());
    in.uptime_ms = 61150U + 1960U + 1961U;
    EXPECT_EQ(mon.validate_wearable_batch(in).canonical_state(), p05::CanonicalState::Stale);

    // Ping-Pong overflow / SD faults: 3 consecutive -> Stop Mode (OPEN-7b)
    in.uptime_ms += 980U;
    in.pingpong_overflow = true;
    EXPECT_EQ(mon.validate_wearable_batch(in).detail_code(), p05::DetailCode::PingPongOverflow);
    EXPECT_TRUE(!mon.stop_mode_requested());
    EXPECT_EQ(mon.validate_wearable_batch(in).detail_code(), p05::DetailCode::PingPongOverflow);
    EXPECT_TRUE(!mon.stop_mode_requested());
    EXPECT_EQ(mon.validate_wearable_batch(in).detail_code(), p05::DetailCode::SpiSdFault);
    EXPECT_TRUE(mon.stop_mode_requested());
    mon.reset_after_wake();
    EXPECT_TRUE(!mon.stop_mode_requested());

    // Battery hysteresis (OPEN-8): < 3400 mV latches, 3500 mV stays latched, >= 3600 mV clears
    // raw = 2513 -> ~4050 mV; raw = 2048 -> ~3300 mV; raw = 2172 -> ~3500 mV; raw = 2234 -> ~3600 mV
    const auto b_ok = mon.validate_battery_adc(true, 2513U);
    EXPECT_TRUE(b_ok.is_ok());
    EXPECT_TRUE(b_ok.value().vbat_mv >= 4045U && b_ok.value().vbat_mv <= 4055U);

    const auto b_low = mon.validate_battery_adc(true, 2048U);
    EXPECT_EQ(b_low.canonical_state(), p05::CanonicalState::SensorUnavailable);
    EXPECT_EQ(b_low.detail_code(), p05::DetailCode::LowBattery);
    EXPECT_TRUE(mon.low_battery_latched());
    EXPECT_TRUE(mon.stop_mode_requested());

    // Still latched at 3.50 V
    EXPECT_EQ(mon.validate_battery_adc(true, 2172U).detail_code(), p05::DetailCode::LowBattery);
    EXPECT_TRUE(mon.low_battery_latched());

    // Recovers at >= 3.60 V
    EXPECT_TRUE(mon.validate_battery_adc(true, 2235U).is_ok());
    EXPECT_TRUE(!mon.low_battery_latched());

    // INA219 I2C failure or timeout > 10 ms MUST return CommsFault, never 0 (R-BND-02 / OPEN-3)
    const auto i_fail = mon.validate_ina219_reading(p05::Ina219RawTransaction{false, 2U, 0U, 0});
    EXPECT_EQ(i_fail.canonical_state(), p05::CanonicalState::CommsFault);
    EXPECT_EQ(i_fail.detail_code(), p05::DetailCode::I2cIna219Fault);
    EXPECT_TRUE(!i_fail.optional_value().has_value());

    const auto i_to =
        mon.validate_ina219_reading(p05::Ina219RawTransaction{true, 11U, 3300U, 5100});
    EXPECT_EQ(i_to.canonical_state(), p05::CanonicalState::CommsFault);

    const auto i_ok =
        mon.validate_ina219_reading(p05::Ina219RawTransaction{true, 1U, 3300U, 5100});
    EXPECT_TRUE(i_ok.is_ok());
    EXPECT_EQ(i_ok.value().bus_mv, 3300U);
    EXPECT_EQ(i_ok.value().current_ua, 5100);

    // microSD 512-byte RAW_ECG_BLOCK & RR_HRV_BLOCK boundary parsing (PROPOSED-SD-01/02)
    auto raw_blk = build_sd_raw_block(7U, 123456U, 0x0010U, 245U, 2048);
    const auto r_raw = p05::parse_sd_raw_ecg_block(
        p05::ConstByteView{raw_blk.data(), raw_blk.size()});
    EXPECT_TRUE(r_raw.is_ok());
    EXPECT_EQ(r_raw.value().header.block_seq, 7U);
    EXPECT_EQ(r_raw.value().valid_sample_count, 245U);
    EXPECT_EQ(r_raw.value().samples[244], 2048);

    // 1-bit flip anywhere in 512 B block -> Malformed / BadCrc (R-BND-01)
    raw_blk[100] ^= 0x04U;
    EXPECT_EQ(
        p05::parse_sd_raw_ecg_block(p05::ConstByteView{raw_blk.data(), raw_blk.size()})
            .detail_code(),
        p05::DetailCode::BadCrc);

    // Wrong block length -> BadLength
    EXPECT_EQ(
        p05::parse_sd_raw_ecg_block(p05::ConstByteView{raw_blk.data(), 511U}).detail_code(),
        p05::DetailCode::BadLength);

    // Valid RR_HRV_BLOCK
    auto rr_blk = build_sd_rr_block(8U, 125000U, 46U, 1U);
    const auto r_rr =
        p05::parse_sd_rr_hrv_block(p05::ConstByteView{rr_blk.data(), rr_blk.size()});
    EXPECT_TRUE(r_rr.is_ok());
    EXPECT_EQ(r_rr.value().valid_rr_count, 46U);
    EXPECT_EQ(r_rr.value().hrv_valid, true);
    EXPECT_EQ(r_rr.value().vbat_mv, 3950U);
    EXPECT_EQ(r_rr.value().hrv.hr_bpm_x100, 7200U);
    EXPECT_EQ(r_rr.value().peaks[45].rr_interval_us, 833000U);

    // Invalid RR count (> 46) with recomputed valid CRC -> SemanticRange
    auto rr_bad = build_sd_rr_block(9U, 126000U, 47U, 1U);
    EXPECT_EQ(
        p05::parse_sd_rr_hrv_block(p05::ConstByteView{rr_bad.data(), rr_bad.size()}).detail_code(),
        p05::DetailCode::SemanticRange);
}

void test_06_code_review_regressions() {
    // Regression 1: Non-consecutive Ping-Pong overflows must NOT falsely trigger Stop Mode,
    // and SPI write fault counter is tracked independently.
    p05::SensorBoundaryMonitor mon;
    std::array<int16_t, p05::kSdMaxRawSamples> ecg{};
    ecg.fill(2048);

    p05::WearableBatchInputs in{};
    in.raw_samples = ecg.data();
    in.sample_count = p05::kSdMaxRawSamples;
    in.fs_hz = 250U;
    in.afe_enabled = true;

    in.uptime_ms = 1000U;
    in.pingpong_overflow = true;
    EXPECT_EQ(mon.validate_wearable_batch(in).detail_code(), p05::DetailCode::PingPongOverflow);
    EXPECT_EQ(mon.consecutive_overflow_faults(), 1U);

    // Valid batch resets consecutive_overflow_faults_
    in.uptime_ms = 1980U;
    in.pingpong_overflow = false;
    EXPECT_TRUE(mon.validate_wearable_batch(in).is_ok());
    EXPECT_EQ(mon.consecutive_overflow_faults(), 0U);

    // Two more scattered overflows interleaved with valid batches never trigger Stop Mode
    in.uptime_ms = 2960U;
    in.pingpong_overflow = true;
    EXPECT_EQ(mon.validate_wearable_batch(in).detail_code(), p05::DetailCode::PingPongOverflow);
    in.uptime_ms = 3940U;
    in.pingpong_overflow = false;
    EXPECT_TRUE(mon.validate_wearable_batch(in).is_ok());
    in.uptime_ms = 4920U;
    in.pingpong_overflow = true;
    EXPECT_EQ(mon.validate_wearable_batch(in).detail_code(), p05::DetailCode::PingPongOverflow);
    EXPECT_TRUE(!mon.stop_mode_requested());

    // Independent SPI write failures: 2 fails + 1 ok resets, 3 consecutive fails triggers Stop Mode
    EXPECT_EQ(mon.record_sd_write_result(false, 20U).detail, p05::DetailCode::SpiSdFault);
    EXPECT_EQ(mon.record_sd_write_result(true, 300U).detail, p05::DetailCode::SpiSdFault);
    EXPECT_EQ(mon.consecutive_spi_faults(), 2U);
    EXPECT_EQ(mon.record_sd_write_result(true, 10U).canonical, p05::CanonicalState::Valid);
    EXPECT_EQ(mon.consecutive_spi_faults(), 0U);
    EXPECT_TRUE(!mon.stop_mode_requested());

    EXPECT_EQ(mon.record_sd_write_result(false, 10U).detail, p05::DetailCode::SpiSdFault);
    EXPECT_EQ(mon.record_sd_write_result(false, 10U).detail, p05::DetailCode::SpiSdFault);
    EXPECT_TRUE(!mon.stop_mode_requested());
    EXPECT_EQ(mon.record_sd_write_result(false, 10U).detail, p05::DetailCode::SpiSdFault);
    EXPECT_TRUE(mon.stop_mode_requested());

    // Regression 2: Retransmitting a frame that failed with SeqGap must NOT report DuplicateIgnored
    p05::HilBoundaryValidator validator;
    const p05::HilSafetyInputs safe_hil{true, true};
    const std::array<int16_t, 4> s = {2000, 2048, 2100, 2050};
    const BuiltFrame cfg = build_hil_config_frame(1U, 250U, 0U, 1U, 1U);
    EXPECT_TRUE(validator.validate_incoming_frame(cfg.view(), 100U, safe_hil).is_ok());

    const BuiltFrame smp2 = build_hil_samples_frame(2U, 0U, s.data(), 4U);
    EXPECT_TRUE(validator.validate_incoming_frame(smp2.view(), 120U, safe_hil).is_ok());

    const BuiltFrame smp4_gap = build_hil_samples_frame(4U, 8U, s.data(), 4U);
    const auto r_gap1 = validator.validate_incoming_frame(smp4_gap.view(), 140U, safe_hil);
    EXPECT_EQ(r_gap1.canonical_state(), p05::CanonicalState::Stale);
    EXPECT_EQ(r_gap1.detail_code(), p05::DetailCode::SeqGap);

    const auto r_gap_retry = validator.validate_incoming_frame(smp4_gap.view(), 150U, safe_hil);
    EXPECT_TRUE(r_gap_retry.is_err());
    EXPECT_EQ(r_gap_retry.canonical_state(), p05::CanonicalState::Stale);
    EXPECT_EQ(r_gap_retry.detail_code(), p05::DetailCode::SeqGap);

    // Regression 3: ProtectionUnknown latch preserves exact DetailCode::ProtectionUnknown
    p05::HilBoundaryValidator v_prot;
    const p05::HilSafetyInputs unknown_prot{false, true};
    const auto r_u1 = v_prot.validate_incoming_frame(cfg.view(), 100U, unknown_prot);
    EXPECT_EQ(r_u1.detail_code(), p05::DetailCode::ProtectionUnknown);
    const auto r_u2 = v_prot.validate_incoming_frame(cfg.view(), 200U, safe_hil);
    EXPECT_EQ(r_u2.detail_code(), p05::DetailCode::ProtectionUnknown);
    EXPECT_EQ(v_prot.check_timeout(300U).detail, p05::DetailCode::ProtectionUnknown);

    // Regression 4: RR_HRV_BLOCK semantic range checks on vbat_mv (> 6600) and ina219_bus_mv (> 26000)
    auto rr_vbat_bad = build_sd_rr_block(10U, 127000U, 1U, 1U);
    p05::write_u16_le(&rr_vbat_bad[18], 7000U);  // impossible on 1:2 3.3V ADC divider
    p05::write_u16_le(&rr_vbat_bad[510], p05::crc16_ccitt_false(rr_vbat_bad.data(), 510U));
    EXPECT_EQ(p05::parse_sd_rr_hrv_block(
                  p05::ConstByteView{rr_vbat_bad.data(), rr_vbat_bad.size()})
                  .detail_code(),
              p05::DetailCode::SemanticRange);

    auto rr_bus_bad = build_sd_rr_block(11U, 128000U, 1U, 1U);
    p05::write_u16_le(&rr_bus_bad[22], 26001U);  // > INA219 26 V max
    p05::write_u16_le(&rr_bus_bad[510], p05::crc16_ccitt_false(rr_bus_bad.data(), 510U));
    EXPECT_EQ(p05::parse_sd_rr_hrv_block(
                  p05::ConstByteView{rr_bus_bad.data(), rr_bus_bad.size()})
                  .detail_code(),
              p05::DetailCode::SemanticRange);
}

}  // namespace

int main() {
    test_01_crc16_and_result_invariants();
    test_02_stateless_hil_parser_valid_and_boundary_rejections();
    test_03_stateful_hil_sequence_gap_duplicate_replay_and_stale();
    test_04_r3_comms_failure_cannot_clear_latched_protection_trip();
    test_05_sensor_boundary_leads_off_battery_ina219_and_sd_blocks();
    test_06_code_review_regressions();

    std::printf("P05 Task Card 1 Host Unit Tests: %d assertions, %d failed\n",
                g_tests_run, g_tests_failed);
    return (g_tests_failed == 0) ? 0 : 1;
}
