#include <array>
#include <atomic>
#include <cstdint>

#include "../HIL/trusted_types.hpp"
#include "platform_controller.hpp"
#include "stm32f103_platform.hpp"
#include "stm32f103_regs.hpp"

namespace {

// Pointer set once at start of main() before enabling interrupts (avoids .init_array reliance).
p05::P05AppController* g_app_ptr = nullptr;

// Lock-free SPSC UART1 RX ring buffer (ISR pushes bytes; main loop frames & parses)
constexpr std::size_t kUartRxRingSize = 256U;
constexpr std::size_t kUartRxRingMask = kUartRxRingSize - 1U;
std::array<uint8_t, kUartRxRingSize> g_uart_rx_ring{};
std::atomic<std::size_t> g_uart_rx_head{0U};
std::atomic<std::size_t> g_uart_rx_tail{0U};
std::atomic<uint8_t> g_wake_exti_flag{0U};

bool pop_uart_rx_byte(uint8_t& out) noexcept {
    const std::size_t tail = g_uart_rx_tail.load(std::memory_order_relaxed);
    const std::size_t head = g_uart_rx_head.load(std::memory_order_acquire);
    if (tail == head) {
        return false;
    }
    out = g_uart_rx_ring[tail];
    g_uart_rx_tail.store((tail + 1U) & kUartRxRingMask, std::memory_order_release);
    return true;
}

// Incremental byte framer for UART1 HIL (0xA5 0x5A + 6B header + PAYLOAD_LEN + 2B CRC)
class UartFrameAssembler {
public:
    bool push_byte(uint8_t b, p05::ConstByteView& complete_frame_out) noexcept {
        if (pos_ == 0U) {
            if (b == p05::kHilSync0) {
                buf_[pos_++] = b;
            }
            return false;
        }
        if (pos_ == 1U) {
            if (b == p05::kHilSync1) {
                buf_[pos_++] = b;
            } else {
                pos_ = (b == p05::kHilSync0) ? 1U : 0U;
            }
            return false;
        }

        buf_[pos_++] = b;
        if (pos_ == p05::kHilHeaderBytes) {
            const uint16_t payload_len = p05::read_u16_le(&buf_[6]);
            expected_len_ = p05::kHilHeaderBytes + static_cast<std::size_t>(payload_len) +
                            p05::kHilCrcBytes;
            if (expected_len_ > p05::kHilMaxFrameBytes) {
                // Deliver oversized header immediately so boundary parser emits BadLength NACK
                complete_frame_out = p05::ConstByteView{buf_.data(), pos_};
                pos_ = 0U;
                return true;
            }
        }

        if (pos_ >= p05::kHilHeaderBytes && pos_ == expected_len_) {
            complete_frame_out = p05::ConstByteView{buf_.data(), pos_};
            pos_ = 0U;
            return true;
        }
        return false;
    }

private:
    std::array<uint8_t, p05::kHilMaxFrameBytes> buf_{};
    std::size_t pos_ = 0U;
    std::size_t expected_len_ = p05::kHilMinFrameBytes;
};

}  // namespace

extern "C" {

void SysTick_Handler() {
    p05::stm32::Stm32F103PlatformHw::isr_on_systick();
}

void DMA1_Channel1_IRQHandler() {
    const uint32_t isr = p05::stm32::dma1()->ISR;
    if ((isr & (1U << 2U)) != 0U) {  // HTIF1 (half 0 ready)
        p05::stm32::dma1()->IFCR = (1U << 2U);
        if (g_app_ptr != nullptr) {
            g_app_ptr->isr_on_dma_half(0U);
        }
    }
    if ((isr & (1U << 1U)) != 0U) {  // TCIF1 (half 1 ready)
        p05::stm32::dma1()->IFCR = (1U << 1U);
        if (g_app_ptr != nullptr) {
            g_app_ptr->isr_on_dma_half(1U);
        }
    }
}

void USART1_IRQHandler() {
    if ((p05::stm32::usart1()->SR & (1U << 5U)) != 0U) {  // RXNE
        const uint8_t b = static_cast<uint8_t>(p05::stm32::usart1()->DR & 0xFFU);
        const std::size_t head = g_uart_rx_head.load(std::memory_order_relaxed);
        const std::size_t next = (head + 1U) & kUartRxRingMask;
        if (next != g_uart_rx_tail.load(std::memory_order_acquire)) {
            g_uart_rx_ring[head] = b;
            g_uart_rx_head.store(next, std::memory_order_release);
        }
    }
}

void EXTI15_10_IRQHandler() {
    if ((p05::stm32::exti()->PR & (1U << 12U)) != 0U) {
        p05::stm32::exti()->PR = (1U << 12U);
        g_wake_exti_flag.store(1U, std::memory_order_release);
    }
}

}  // extern "C"

int main() {
    // Static Zero-Heap firmware instances initialized deterministically in main() before IRQs
    static p05::stm32::Stm32F103PlatformHw hw;
    static p05::P05AppController app(hw);
    g_app_ptr = &app;

    UartFrameAssembler framer;
    app.boot(p05::PowerClockProfile::Run8MHzQ15Sleep);

    while (true) {
        if (g_wake_exti_flag.exchange(0U, std::memory_order_acq_rel) != 0U) {
            if (app.mode() == p05::AppMode::StopMode) {
                app.boot(p05::PowerClockProfile::Run8MHzQ15Sleep);
            } else {
                app.request_user_stop();
            }
        }

        uint8_t rx_byte = 0U;
        while (pop_uart_rx_byte(rx_byte)) {
            p05::ConstByteView frame{};
            if (framer.push_byte(rx_byte, frame)) {
                app.on_uart1_frame(frame);
            }
        }

        app.step();
    }
}
