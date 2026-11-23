#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "../HIL/hil_session.hpp"
#include "../HIL/trusted_types.hpp"

namespace {

inline constexpr double kPi = 3.14159265358979323846;

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

struct MockCycleClock {
    uint32_t current = 0xFFFFFF00U;  // Start near 32-bit wraparound to verify wrap safety
    uint32_t calls = 0U;
    uint32_t step_pattern[3] = {120U, 150U, 180U};
    uint32_t sample_idx = 0U;

    static uint32_t read(void* ctx) noexcept {
        auto* self = static_cast<MockCycleClock*>(ctx);
        const uint32_t val = self->current;
        if ((self->calls % 2U) == 0U) {
            // Between start and end of sample, advance by deterministic cycle count
            self->current += self->step_pattern[self->sample_idx % 3U];
            ++self->sample_idx;
        }
        ++self->calls;
        return val;
    }
};

struct BuiltReq {
    std::array<uint8_t, p05::kHilMaxFrameBytes> bytes{};
    std::size_t length = 0U;
    p05::ConstByteView view() const noexcept { return p05::ConstByteView{bytes.data(), length}; }
};

BuiltReq make_config_req(uint16_t seq,
                         uint16_t fs_hz,
                         uint8_t math_mode,
                         uint8_t interp_en,
                         uint8_t reset_state) noexcept {
    BuiltReq f{};
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

BuiltReq make_samples_req(uint16_t seq,
                          uint32_t first_idx,
                          const int16_t* samples,
                          uint8_t count) noexcept {
    BuiltReq f{};
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

struct DecodedAck {
    bool valid_wire = false;
    uint16_t ack_seq = 0U;
    p05::CanonicalState canonical = p05::CanonicalState::ProtectionStatusUnknown;
    p05::DetailCode detail = p05::DetailCode::ProtectionUnknown;
    uint32_t dwt_min = 0U;
    uint32_t dwt_mean = 0U;
    uint32_t dwt_max = 0U;
    uint8_t peak_count = 0U;
    std::array<p05::TrustedRPeakRecord, p05::kMaxPeaksPerBatch> peaks{};
    bool hrv_valid = false;
    p05::TrustedHrvMetrics hrv{};
};

struct DecodedNack {
    bool valid_wire = false;
    uint16_t rejected_seq = 0U;
    p05::CanonicalState canonical = p05::CanonicalState::ProtectionStatusUnknown;
    p05::DetailCode detail = p05::DetailCode::ProtectionUnknown;
};

DecodedAck decode_ack(const p05::EncodedHilFrame& f) noexcept {
    DecodedAck d{};
    if (f.length < p05::kHilHeaderBytes + 34U + p05::kHilCrcBytes) {
        return d;
    }
    if (f.bytes[0] != p05::kHilSync0 || f.bytes[1] != p05::kHilSync1 ||
        f.bytes[2] != p05::kHilVersion ||
        f.bytes[3] != static_cast<uint8_t>(p05::HilMsgType::ResultAck)) {
        return d;
    }
    const uint16_t payload_len = p05::read_u16_le(&f.bytes[6]);
    if (f.length != p05::kHilHeaderBytes + static_cast<std::size_t>(payload_len) + p05::kHilCrcBytes) {
        return d;
    }
    const std::size_t crc_off = p05::kHilHeaderBytes + static_cast<std::size_t>(payload_len);
    if (p05::read_u16_le(&f.bytes[crc_off]) != p05::crc16_ccitt_false(f.bytes.data(), crc_off)) {
        return d;
    }

    std::size_t off = p05::kHilHeaderBytes;
    d.ack_seq = p05::read_u16_le(&f.bytes[off]);
    off += 2U;
    d.canonical = static_cast<p05::CanonicalState>(f.bytes[off++]);
    d.detail = static_cast<p05::DetailCode>(f.bytes[off++]);
    d.dwt_min = p05::read_u32_le(&f.bytes[off]);
    off += 4U;
    d.dwt_mean = p05::read_u32_le(&f.bytes[off]);
    off += 4U;
    d.dwt_max = p05::read_u32_le(&f.bytes[off]);
    off += 4U;
    d.peak_count = f.bytes[off++];
    if (d.peak_count > p05::kMaxPeaksPerBatch ||
        payload_len != static_cast<uint16_t>(34U + (10U * static_cast<uint16_t>(d.peak_count)))) {
        return d;
    }

    for (std::size_t i = 0U; i < static_cast<std::size_t>(d.peak_count); ++i) {
        d.peaks[i].peak_sample_idx = p05::read_u32_le(&f.bytes[off]);
        off += 4U;
        d.peaks[i].sub_sample_offset_q15 = p05::read_i16_le(&f.bytes[off]);
        off += 2U;
        d.peaks[i].rr_interval_us = p05::read_u32_le(&f.bytes[off]);
        off += 4U;
    }

    d.hrv_valid = (f.bytes[off++] == 1U);
    d.hrv.mean_rr_us = p05::read_u32_le(&f.bytes[off]);
    off += 4U;
    d.hrv.hr_bpm_x100 = p05::read_u16_le(&f.bytes[off]);
    off += 2U;
    d.hrv.sdnn_us = p05::read_u32_le(&f.bytes[off]);
    off += 4U;
    d.hrv.rmssd_us = p05::read_u32_le(&f.bytes[off]);
    off += 4U;
    d.hrv.pnn50_permille = p05::read_u16_le(&f.bytes[off]);
    off += 2U;

    d.valid_wire = (off == crc_off);
    return d;
}

DecodedNack decode_nack(const p05::EncodedHilFrame& f) noexcept {
    DecodedNack d{};
    if (f.length != 14U) {
        return d;
    }
    if (f.bytes[0] != p05::kHilSync0 || f.bytes[1] != p05::kHilSync1 ||
        f.bytes[2] != p05::kHilVersion ||
        f.bytes[3] != static_cast<uint8_t>(p05::HilMsgType::ErrorNack)) {
        return d;
    }
    if (p05::read_u16_le(&f.bytes[6]) != 4U) {
        return d;
    }
    if (p05::read_u16_le(&f.bytes[12]) != p05::crc16_ccitt_false(f.bytes.data(), 12U)) {
        return d;
    }
    d.rejected_seq = p05::read_u16_le(&f.bytes[8]);
    d.canonical = static_cast<p05::CanonicalState>(f.bytes[10]);
    d.detail = static_cast<p05::DetailCode>(f.bytes[11]);
    d.valid_wire = true;
    return d;
}

int16_t synth_ecg_adc(double t_sec, const double* beats, std::size_t n_beats) {
    double val = 2048.0 + (150.0 * std::sin(2.0 * kPi * 50.0 * t_sec)) +
                 (200.0 * std::sin(2.0 * kPi * 0.3 * t_sec));
    for (std::size_t b = 0U; b < n_beats; ++b) {
        const double dt = t_sec - beats[b];
        if (dt < -0.35 || dt > 0.45) {
            continue;
        }
        val += 90.0 * std::exp(-0.5 * ((dt + 0.16) / 0.022) * ((dt + 0.16) / 0.022));
        val -= 140.0 * std::exp(-0.5 * ((dt + 0.024) / 0.008) * ((dt + 0.024) / 0.008));
        val += 1100.0 * std::exp(-0.5 * (dt / 0.011) * (dt / 0.011));
        val -= 220.0 * std::exp(-0.5 * ((dt - 0.024) / 0.009) * ((dt - 0.024) / 0.009));
        val += 240.0 * std::exp(-0.5 * ((dt - 0.26) / 0.045) * ((dt - 0.26) / 0.045));
    }
    if (val < 0.0) {
        val = 0.0;
    } else if (val > 4095.0) {
        val = 4095.0;
    }
    return static_cast<int16_t>(std::lround(val));
}

void test_01_end_to_end_hil_pipeline_q15_250hz_and_float_500hz() {
    std::array<double, 16U> beats{};
    for (std::size_t i = 0U; i < beats.size(); ++i) {
        beats[i] = 1.0 + (static_cast<double>(i) * (60.0 / 72.0));  // exact 72.00 BPM
    }

    const p05::HilSafetyInputs safe_hil{true, true};

    struct RunCase {
        uint16_t fs;
        uint8_t math_mode;
        uint8_t interp;
    };
    const std::array<RunCase, 3U> cases = {{
        {250U, 0U, 1U},  // 250 Hz Q15 + Parabolic ON
        {250U, 0U, 0U},  // 250 Hz Q15 + Parabolic OFF (Ablation 2)
        {500U, 1U, 1U},  // 500 Hz Soft-Float + Parabolic ON (Ablation 1 & 2)
    }};

    for (const RunCase& tc : cases) {
        MockCycleClock clk{};
        p05::HilSession session(&MockCycleClock::read, &clk);

        uint16_t seq = 1U;
        uint32_t uptime = 100U;
        const BuiltReq cfg = make_config_req(seq, tc.fs, tc.math_mode, tc.interp, 1U);
        const DecodedAck ack0 = decode_ack(session.handle_frame(cfg.view(), uptime, safe_hil));
        EXPECT_TRUE(ack0.valid_wire);
        EXPECT_EQ(ack0.ack_seq, 1U);
        EXPECT_EQ(ack0.canonical, p05::CanonicalState::Valid);
        EXPECT_EQ(ack0.detail, p05::DetailCode::Ok);
        EXPECT_EQ(ack0.peak_count, 0U);
        EXPECT_TRUE(!ack0.hrv_valid);
        EXPECT_TRUE(session.active());

        const uint32_t total_samples = static_cast<uint32_t>(tc.fs) * 15U;
        std::size_t total_peaks = 0U;
        bool saw_nonzero_offset = false;
        bool saw_hrv_valid = false;
        DecodedAck last_ack{};

        for (uint32_t idx = 0U; idx < total_samples; idx += p05::kMaxBatchSamples) {
            ++seq;
            uptime += 50U;
            const uint32_t remain = total_samples - idx;
            const uint8_t count = static_cast<uint8_t>(
                (remain < p05::kMaxBatchSamples) ? remain : p05::kMaxBatchSamples);

            std::array<int16_t, p05::kMaxBatchSamples> batch{};
            for (uint8_t i = 0U; i < count; ++i) {
                const double t = static_cast<double>(idx + i) / static_cast<double>(tc.fs);
                batch[i] = synth_ecg_adc(t, beats.data(), beats.size());
            }

            const BuiltReq req = make_samples_req(seq, idx, batch.data(), count);
            const DecodedAck ack = decode_ack(session.handle_frame(req.view(), uptime, safe_hil));
            EXPECT_TRUE(ack.valid_wire);
            EXPECT_EQ(ack.ack_seq, seq);
            EXPECT_EQ(ack.canonical, p05::CanonicalState::Valid);

            for (uint8_t p = 0U; p < ack.peak_count; ++p) {
                ++total_peaks;
                if (ack.peaks[p].sub_sample_offset_q15 != 0) {
                    saw_nonzero_offset = true;
                }
                if (tc.interp == 0U) {
                    EXPECT_EQ(ack.peaks[p].sub_sample_offset_q15, 0);
                }
            }
            if (ack.hrv_valid) {
                saw_hrv_valid = true;
            }
            last_ack = ack;
        }

        EXPECT_EQ(total_peaks, 16U);
        EXPECT_TRUE(saw_hrv_valid);
        EXPECT_TRUE(last_ack.hrv_valid);
        // Verified mock DWT min=120, mean=150, max=180 across 32-bit wraparound!
        EXPECT_EQ(last_ack.dwt_min, 120U);
        EXPECT_EQ(last_ack.dwt_mean, 150U);
        EXPECT_EQ(last_ack.dwt_max, 180U);
        // Verified end-to-end HR around 72.00 BPM (71.90 .. 72.10 BPM) & Mean RR ~833.3 ms
        EXPECT_TRUE(last_ack.hrv.hr_bpm_x100 >= 7190U && last_ack.hrv.hr_bpm_x100 <= 7210U);
        EXPECT_TRUE(last_ack.hrv.mean_rr_us >= 832000U && last_ack.hrv.mean_rr_us <= 834600U);
        if (tc.interp == 1U) {
            EXPECT_TRUE(saw_nonzero_offset);
        }
    }
}

void test_02_duplicate_replay_gap_malformed_and_timeout_policies() {
    MockCycleClock clk{};
    p05::HilSession session(&MockCycleClock::read, &clk);
    const p05::HilSafetyInputs safe_hil{true, true};
    const std::array<int16_t, 4U> samples = {2048, 2050, 2046, 2048};

    // Start session
    const BuiltReq cfg = make_config_req(10U, 250U, 0U, 1U, 1U);
    EXPECT_TRUE(decode_ack(session.handle_frame(cfg.view(), 100U, safe_hil)).valid_wire);

    // Send SEQ=11 -> normal ACK
    const BuiltReq s11 = make_samples_req(11U, 0U, samples.data(), 4U);
    const DecodedAck ack11 = decode_ack(session.handle_frame(s11.view(), 120U, safe_hil));
    EXPECT_TRUE(ack11.valid_wire);
    EXPECT_EQ(ack11.detail, p05::DetailCode::Ok);
    const uint32_t calls_after_11 = clk.calls;

    // Duplicate SEQ=11 -> re-emits cached ACK with DuplicateIgnored WITHOUT re-running DSP!
    const DecodedAck ack11_dup = decode_ack(session.handle_frame(s11.view(), 130U, safe_hil));
    EXPECT_TRUE(ack11_dup.valid_wire);
    EXPECT_EQ(ack11_dup.ack_seq, 11U);
    EXPECT_EQ(ack11_dup.canonical, p05::CanonicalState::Valid);
    EXPECT_EQ(ack11_dup.detail, p05::DetailCode::DuplicateIgnored);
    EXPECT_EQ(clk.calls, calls_after_11);  // DSP did NOT run again!

    // Replay SEQ=10 -> HIL_ERROR_NACK (Malformed / SeqReplay)
    const BuiltReq s10 = make_samples_req(10U, 0U, samples.data(), 4U);
    const DecodedNack nack_rep = decode_nack(session.handle_frame(s10.view(), 140U, safe_hil));
    EXPECT_TRUE(nack_rep.valid_wire);
    EXPECT_EQ(nack_rep.rejected_seq, 10U);
    EXPECT_EQ(nack_rep.canonical, p05::CanonicalState::Malformed);
    EXPECT_EQ(nack_rep.detail, p05::DetailCode::SeqReplay);

    // Gap SEQ=15 (skipped 12..14) -> HIL_ERROR_NACK (Stale / SeqGap) + engages 2 s blanking!
    const BuiltReq s15 = make_samples_req(15U, 16U, samples.data(), 4U);
    const DecodedNack nack_gap = decode_nack(session.handle_frame(s15.view(), 150U, safe_hil));
    EXPECT_TRUE(nack_gap.valid_wire);
    EXPECT_EQ(nack_gap.rejected_seq, 15U);
    EXPECT_EQ(nack_gap.canonical, p05::CanonicalState::Stale);
    EXPECT_EQ(nack_gap.detail, p05::DetailCode::SeqGap);
    EXPECT_TRUE(session.in_gap_blanking());

    // OPEN-4: 3 consecutive malformed frames (bad CRC) abort HIL session and reset stats
    BuiltReq bad_crc = make_samples_req(16U, 20U, samples.data(), 4U);
    bad_crc.bytes[bad_crc.length - 1U] ^= 0x80U;
    EXPECT_EQ(decode_nack(session.handle_frame(bad_crc.view(), 160U, safe_hil)).detail,
              p05::DetailCode::BadCrc);
    EXPECT_TRUE(session.active());
    EXPECT_EQ(decode_nack(session.handle_frame(bad_crc.view(), 170U, safe_hil)).detail,
              p05::DetailCode::BadCrc);
    EXPECT_TRUE(session.active());
    EXPECT_EQ(decode_nack(session.handle_frame(bad_crc.view(), 180U, safe_hil)).detail,
              p05::DetailCode::BadCrc);
    EXPECT_TRUE(!session.active());
    EXPECT_EQ(session.cycle_stats().sample_count, 0U);

    // OPEN-3 / OPEN-5: UART1 idle timeout (> 2000 ms) closes session and resets DSP
    const BuiltReq cfg2 = make_config_req(1U, 250U, 0U, 1U, 1U);
    EXPECT_TRUE(decode_ack(session.handle_frame(cfg2.view(), 1000U, safe_hil)).valid_wire);
    EXPECT_EQ(session.poll_timeout(3000U).canonical, p05::CanonicalState::Valid);
    const auto to_st = session.poll_timeout(3001U);
    EXPECT_EQ(to_st.canonical, p05::CanonicalState::CommsFault);
    EXPECT_EQ(to_st.detail, p05::DetailCode::UartFault);
    EXPECT_TRUE(!session.active());
}

void test_03_safety_lockout_and_r3_invariance_in_hil_session() {
    p05::HilSession session;
    const BuiltReq cfg = make_config_req(1U, 250U, 0U, 1U, 1U);

    // Electrodes attached while HIL_CONFIG_REQ arrives -> NACK ProtectionStatusUnknown + latched lockout
    const p05::HilSafetyInputs attached{true, false};
    const DecodedNack nack_lock = decode_nack(session.handle_frame(cfg.view(), 500U, attached));
    EXPECT_TRUE(nack_lock.valid_wire);
    EXPECT_EQ(nack_lock.canonical, p05::CanonicalState::ProtectionStatusUnknown);
    EXPECT_EQ(nack_lock.detail, p05::DetailCode::ModeConflictLockout);
    EXPECT_TRUE(session.lockout_latched());
    EXPECT_TRUE(!session.active());

    // UART comms fault MUST NOT clear latched protection lockout (R-BND-03)
    EXPECT_EQ(session.on_uart_fault().canonical, p05::CanonicalState::CommsFault);
    EXPECT_TRUE(session.lockout_latched());

    // Even after detaching electrodes, frames remain blocked until explicit try_clear_lockout()
    const p05::HilSafetyInputs detached{true, true};
    const DecodedNack nack_still = decode_nack(session.handle_frame(cfg.view(), 600U, detached));
    EXPECT_TRUE(nack_still.valid_wire);
    EXPECT_EQ(nack_still.canonical, p05::CanonicalState::ProtectionStatusUnknown);

    EXPECT_TRUE(!session.try_clear_lockout(attached));
    EXPECT_TRUE(session.try_clear_lockout(detached));
    EXPECT_TRUE(!session.lockout_latched());
    EXPECT_TRUE(decode_ack(session.handle_frame(cfg.view(), 700U, detached)).valid_wire);
    EXPECT_TRUE(session.active());
}

}  // namespace

int main() {
    test_01_end_to_end_hil_pipeline_q15_250hz_and_float_500hz();
    test_02_duplicate_replay_gap_malformed_and_timeout_policies();
    test_03_safety_lockout_and_r3_invariance_in_hil_session();

    std::printf("P05 Task Card 4 HIL Session Integration Tests: %d assertions, %d failed\n",
                g_tests_run, g_tests_failed);
    return (g_tests_failed == 0) ? 0 : 1;
}
