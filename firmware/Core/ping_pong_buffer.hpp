#ifndef P05_CORE_PING_PONG_BUFFER_HPP
#define P05_CORE_PING_PONG_BUFFER_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "../HIL/trusted_types.hpp"

namespace p05 {

// Lock-free ISR-to-main Ping-Pong double buffer for Timer-triggered ADC1 + DMA1_Ch1 (R-SD-01).
// - Total SRAM: 2 x 245 x 2 B = 980 B (OPEN-7a, matches 1 RAW_ECG_BLOCK exactly).
// - ISR does ZERO DSP and ZERO SPI1 work; it only updates atomic readiness & overflow flags.
class PingPongDmaBuffer {
public:
    static constexpr std::size_t kHalfSamples = kSdMaxRawSamples;      // 245
    static constexpr std::size_t kTotalSamples = 2U * kHalfSamples;    // 490

    struct ReadyHalf {
        bool ready = false;
        uint8_t half_index = 0U;  // 0 = first half [0..244], 1 = second half [245..489]
        uint32_t timestamp_ms = 0U;
        bool overflowed = false;
        const int16_t* samples = nullptr;  // Owner: PingPongDmaBuffer; length == kHalfSamples
    };

    void reset() noexcept {
        dma_buffer_.fill(0);
        ready_mask_.store(0U, std::memory_order_relaxed);
        overflow_flag_.store(0U, std::memory_order_relaxed);
        last_half_ready_.store(0U, std::memory_order_relaxed);
        ts_ms_[0].store(0U, std::memory_order_relaxed);
        ts_ms_[1].store(0U, std::memory_order_relaxed);
    }

    // Raw destination pointer for DMA1_Channel1->CMAR (490 x int16_t circular buffer).
    int16_t* dma_dest_ptr() noexcept { return dma_buffer_.data(); }

    // Called from DMA1_Channel1_IRQHandler on Half-Transfer (HTIF, half_index = 0)
    // or Transfer-Complete (TCIF, half_index = 1). Strictly O(1), non-blocking!
    void isr_notify_half_complete(uint8_t half_index, uint32_t uptime_ms) noexcept {
        const uint8_t bit = static_cast<uint8_t>(1U << (half_index & 1U));
        const uint8_t prev = ready_mask_.fetch_or(bit, std::memory_order_release);
        if ((prev & bit) != 0U) {
            // Main loop did not finish consuming this half before DMA wrapped around!
            overflow_flag_.store(1U, std::memory_order_release);
        }
        ts_ms_[half_index & 1U].store(uptime_ms, std::memory_order_release);
        last_half_ready_.store(static_cast<uint8_t>(half_index & 1U), std::memory_order_release);
    }

    // Called from main loop outside ISR to claim the oldest ready half-buffer.
    ReadyHalf main_poll_ready() noexcept {
        ReadyHalf out{};
        const uint8_t mask = ready_mask_.load(std::memory_order_acquire);
        if (mask == 0U) {
            return out;
        }

        // If both halves are pending, consume the older half first (FIFO order)
        uint8_t pick = 0U;
        if (mask == 0x03U) {
            const uint8_t newest = last_half_ready_.load(std::memory_order_acquire);
            pick = static_cast<uint8_t>(newest ^ 1U);
        } else {
            pick = (mask == 0x01U) ? 0U : 1U;
        }

        out.ready = true;
        out.half_index = pick;
        out.timestamp_ms = ts_ms_[pick].load(std::memory_order_acquire);
        out.overflowed = (overflow_flag_.exchange(0U, std::memory_order_acq_rel) != 0U);
        out.samples = &dma_buffer_[static_cast<std::size_t>(pick) * kHalfSamples];

        // Clear ready bit after copying/claiming metadata
        const uint8_t clear_mask = static_cast<uint8_t>(~(1U << pick));
        ready_mask_.fetch_and(clear_mask, std::memory_order_acq_rel);
        return out;
    }

private:
    std::array<int16_t, kTotalSamples> dma_buffer_{};
    std::atomic<uint8_t> ready_mask_{0U};
    std::atomic<uint8_t> overflow_flag_{0U};
    std::atomic<uint8_t> last_half_ready_{0U};
    std::array<std::atomic<uint32_t>, 2U> ts_ms_{};
};

}  // namespace p05

#endif  // P05_CORE_PING_PONG_BUFFER_HPP
