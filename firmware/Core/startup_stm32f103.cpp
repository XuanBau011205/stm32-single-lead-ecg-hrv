#include <cstdint>

extern "C" {

extern uint32_t _estack;
extern uint32_t _sidata;
extern uint32_t _sdata;
extern uint32_t _edata;
extern uint32_t _sbss;
extern uint32_t _ebss;

using InitFn = void (*)();
extern InitFn __preinit_array_start[];
extern InitFn __preinit_array_end[];
extern InitFn __init_array_start[];
extern InitFn __init_array_end[];

void p05_firmware_main();

void Default_Handler() {
    while (true) {
    }
}

void Reset_Handler();
void NMI_Handler() __attribute__((weak, alias("Default_Handler")));
void HardFault_Handler() __attribute__((weak, alias("Default_Handler")));
void MemManage_Handler() __attribute__((weak, alias("Default_Handler")));
void BusFault_Handler() __attribute__((weak, alias("Default_Handler")));
void UsageFault_Handler() __attribute__((weak, alias("Default_Handler")));
void SVC_Handler() __attribute__((weak, alias("Default_Handler")));
void DebugMon_Handler() __attribute__((weak, alias("Default_Handler")));
void PendSV_Handler() __attribute__((weak, alias("Default_Handler")));
void SysTick_Handler() __attribute__((weak, alias("Default_Handler")));
void DMA1_Channel1_IRQHandler() __attribute__((weak, alias("Default_Handler")));
void USART1_IRQHandler() __attribute__((weak, alias("Default_Handler")));
void EXTI15_10_IRQHandler() __attribute__((weak, alias("Default_Handler")));

using IsrFn = void (*)();

// STM32F103C8T6 Cortex-M3 Vector Table (16 core vectors + 43 NVIC IRQs covering EXTI15_10 IRQ40)
__attribute__((section(".isr_vector"), used))
const IsrFn g_pfnVectors[16 + 43] = {
    reinterpret_cast<IsrFn>(&_estack),  // 0: Initial Stack Pointer (0x20005000)
    Reset_Handler,                      // 1: Reset
    NMI_Handler,                        // 2
    HardFault_Handler,                  // 3
    MemManage_Handler,                  // 4
    BusFault_Handler,                   // 5
    UsageFault_Handler,                 // 6
    nullptr, nullptr, nullptr, nullptr, // 7..10
    SVC_Handler,                        // 11
    DebugMon_Handler,                   // 12
    nullptr,                            // 13
    PendSV_Handler,                     // 14
    SysTick_Handler,                    // 15
    // NVIC IRQ 0..42
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,  // 0..3
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,  // 4..7
    Default_Handler, Default_Handler, Default_Handler,                   // 8..10
    DMA1_Channel1_IRQHandler,                                            // 11: DMA1_Ch1
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,  // 12..15
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,  // 16..19
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,  // 20..23
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,  // 24..27
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,  // 28..31
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,  // 32..35
    Default_Handler,                                                     // 36
    USART1_IRQHandler,                                                   // 37: USART1
    Default_Handler, Default_Handler,                                    // 38..39
    EXTI15_10_IRQHandler,                                                // 40: EXTI15_10 (PB12)
    Default_Handler, Default_Handler,                                    // 41..42
};

inline void run_init_array(InitFn* start, InitFn* end) {
    const uintptr_t stop = reinterpret_cast<uintptr_t>(end);
    for (uint32_t i = 0U; reinterpret_cast<uintptr_t>(&start[i]) < stop; ++i) {
        if (start[i] != nullptr) {
            start[i]();
        }
    }
}

void Reset_Handler() {
    uint32_t* src = &_sidata;
    uint32_t* dst = &_sdata;
    const uintptr_t edata_addr = reinterpret_cast<uintptr_t>(&_edata);
    while (reinterpret_cast<uintptr_t>(dst) < edata_addr) {
        *dst++ = *src++;
    }
    dst = &_sbss;
    const uintptr_t ebss_addr = reinterpret_cast<uintptr_t>(&_ebss);
    while (reinterpret_cast<uintptr_t>(dst) < ebss_addr) {
        *dst++ = 0U;
    }
    run_init_array(__preinit_array_start, __preinit_array_end);
    run_init_array(__init_array_start, __init_array_end);
    p05_firmware_main();
    while (true) {
    }
}

}  // extern "C"
