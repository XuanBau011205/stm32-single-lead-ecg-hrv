#ifndef P05_HIL_HIL_SESSION_HPP
#define P05_HIL_HIL_SESSION_HPP

#include <array>
#include <cstddef>
#include <cstdint>

#include "../DSP/hrv_analyzer.hpp"
#include "../DSP/pan_tompkins.hpp"
#include "boundary_parser.hpp"
#include "trusted_types.hpp"

namespace p05 {

// Maximum HIL_RESULT_ACK frame size:
// 8 B header + 34 B fixed payload + (16 * 10 B peaks) + 2 B CRC = 204 B.
inline constexpr std::size_t kHilMaxResponseBytes =
    kHilHeaderBytes + 34U + (10U * kMaxPeaksPerBatch) + kHilCrcBytes;

// Static Zero-Heap encoded wire frame ready for UART1 TX.
struct EncodedHilFrame {
    std::array<uint8_t, kHilMaxResponseBytes> bytes{};
    std::size_t length = 0U;

    ConstByteView view() const noexcept { return ConstByteView{bytes.data(), length}; }
};

// Chip-independent cycle counter callback (Platform Adapter injects DWT->CYCCNT reader in Card 5).
using CycleCounterFn = uint32_t (*)(void* ctx) noexcept;

struct CycleStats {
    uint32_t min_cycles = 0U;
    uint32_t mean_cycles = 0U;
    uint32_t max_cycles = 0U;
    uint64_t total_cycles = 0U;
    uint32_t sample_count = 0U;

    void reset() noexcept {
        min_cycles = 0U;
        mean_cycles = 0U;
        max_cycles = 0U;
        total_cycles = 0U;
        sample_count = 0U;
    }

    void record(uint32_t cycles) noexcept {
        if (sample_count == 0U || cycles < min_cycles) {
            min_cycles = cycles;
        }
        if (sample_count == 0U || cycles > max_cycles) {
            max_cycles = cycles;
        }
        total_cycles += static_cast<uint64_t>(cycles);
        ++sample_count;
        mean_cycles = static_cast<uint32_t>(
            (total_cycles + (static_cast<uint64_t>(sample_count) / 2ULL)) /
            static_cast<uint64_t>(sample_count));
    }
};

// Full hardware-independent UART1 HIL protocol session controller (Task Card 4):
// Chains Boundary Parser (Card 1) -> EcgDspCore (Card 2) -> HrvAnalyzer (Card 3)
// -> Little-Endian binary HIL_RESULT_ACK / HIL_ERROR_NACK encoder with CRC-16/CCITT-FALSE.
// Zero HAL, Zero Heap, C++17 compliant.
class HilSession {
public:
    explicit HilSession(CycleCounterFn cycle_fn = nullptr, void* cycle_ctx = nullptr) noexcept;

    void set_cycle_counter(CycleCounterFn cycle_fn, void* cycle_ctx) noexcept;

    // Processes one untrusted wire frame received from UART1 and returns an encoded
    // HIL_RESULT_ACK (0x81) or HIL_ERROR_NACK (0x8F) response frame.
    EncodedHilFrame handle_frame(ConstByteView raw_frame,
                                 uint32_t uptime_ms,
                                 const HilSafetyInputs& safety) noexcept;

    // Checks UART1 idle timeout (> 2000 ms, OPEN-3 / OPEN-5). Closes session and resets DSP on timeout.
    FailureStatus poll_timeout(uint32_t uptime_ms) noexcept;

    // Records transport fault on UART1. Closes session and resets DSP/HRV per OPEN-3,
    // while NEVER clearing a latched protection lockout (R-BND-03).
    FailureStatus on_uart_fault() noexcept;

    // Clears latched protection lockout only when hardware reports verified safe state.
    bool try_clear_lockout(const HilSafetyInputs& safety) noexcept;

    bool active() const noexcept { return validator_.hil_session_active(); }
    bool lockout_latched() const noexcept { return validator_.lockout_latched(); }
    const CycleStats& cycle_stats() const noexcept { return cycles_; }
    const DspConfig& dsp_config() const noexcept { return dsp_.config(); }
    bool in_gap_blanking() const noexcept { return dsp_.in_gap_blanking(); }

    // Shared DSP/HRV accessors for mutually exclusive Wearable mode (saves ~2.7 KB SRAM).
    EcgDspCore& shared_dsp() noexcept { return dsp_; }
    const EcgDspCore& shared_dsp() const noexcept { return dsp_; }
    HrvAnalyzer& shared_hrv() noexcept { return hrv_; }
    const HrvAnalyzer& shared_hrv() const noexcept { return hrv_; }

private:
    static EncodedHilFrame encode_error_nack(uint16_t rejected_seq,
                                             FailureStatus status) noexcept;
    static EncodedHilFrame encode_result_ack(
        uint16_t ack_seq,
        FailureStatus status,
        const CycleStats& stats,
        const std::array<DetectedRPeak, kMaxPeaksPerBatch>& peaks,
        uint8_t peak_count,
        const Result<TrustedHrvMetrics>& hrv_res) noexcept;

    void reset_processing_state() noexcept;
    uint32_t read_cycles() const noexcept;

    HilBoundaryValidator validator_{};
    EcgDspCore dsp_{};
    HrvAnalyzer hrv_{};
    CycleCounterFn cycle_fn_ = nullptr;
    void* cycle_ctx_ = nullptr;
    CycleStats cycles_{};

    // Cached last valid ACK fields for exact DuplicateIgnored replay without re-running DSP
    bool has_cached_ack_ = false;
    uint16_t cached_ack_seq_ = 0U;
    CycleStats cached_cycles_{};
    std::array<DetectedRPeak, kMaxPeaksPerBatch> cached_peaks_{};
    uint8_t cached_peak_count_ = 0U;
    Result<TrustedHrvMetrics> cached_hrv_ =
        Result<TrustedHrvMetrics>::fail(CanonicalState::SensorUnavailable, DetailCode::LeadsOff);
};

}  // namespace p05

#endif  // P05_HIL_HIL_SESSION_HPP
