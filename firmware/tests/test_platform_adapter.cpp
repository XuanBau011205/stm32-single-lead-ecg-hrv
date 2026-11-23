#include <array>
#include <cstdint>
#include <cstdio>

#include "../Core/ping_pong_buffer.hpp"
#include "../Core/platform_controller.hpp"
#include "../Core/sd_block_formatter.hpp"
#include "../Core/sensor_storage_boundary.hpp"
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

enum class HwEvent : uint8_t {
    StopAdcDma = 1U,
    Spi1PinsAnalog = 2U,
    SdPowerOff = 3U,
    Ina219PowerDown = 4U,
    AfeOff = 5U,
    BatSenseOff = 6U,
    UnusedGpioAnalog = 7U,
    EnterStopWfi = 8U,
};

class MockPlatformHw final : public p05::IPlatformHw {
public:
    uint32_t now_ms = 1000U;
    uint32_t cycles = 1000U;
    bool lod_p = false;
    bool lod_n = false;
    bool bat_adc_ok = true;
    uint16_t bat_raw = 2450U;  // ~3950 mV
    bool sd_init_ok = true;
    bool sd_write_ok = true;
    uint32_t sd_write_dur_ms = 12U;
    p05::Ina219RawTransaction ina_tx{true, 1U, 3300U, 5100};
    bool ina_pd_ok = true;

    bool afe_on = false;
    bool sd_pwr_on = false;
    bool bat_en = false;
    bool adc_dma_running = false;
    uint16_t adc_fs_hz = 0U;
    int16_t* dma_dst = nullptr;
    uint32_t sleep_wfi_calls = 0U;
    uint32_t stop_wfi_calls = 0U;
    uint32_t ina_pd_calls = 0U;
    uint32_t sd_write_calls = 0U;

    std::array<p05::SdSectorBuffer, 16U> sd_sectors{};
    std::size_t sd_stored = 0U;

    std::array<uint8_t, p05::kHilMaxResponseBytes> last_uart_tx{};
    std::size_t last_uart_len = 0U;

    std::array<HwEvent, 32U> event_log{};
    std::size_t event_count = 0U;

    void clear_events() noexcept { event_count = 0U; }

    void log_event(HwEvent ev) noexcept {
        if (event_count < event_log.size()) {
            event_log[event_count++] = ev;
        }
    }

    int find_event(HwEvent ev) const noexcept {
        for (std::size_t i = 0U; i < event_count; ++i) {
            if (event_log[i] == ev) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    uint32_t uptime_ms() const noexcept override { return now_ms; }
    uint32_t dwt_cycles() const noexcept override { return cycles; }

    void set_clock_profile(p05::PowerClockProfile) noexcept override {}
    void enter_sleep_wfi() noexcept override { ++sleep_wfi_calls; }

    void configure_unused_gpio_analog_keep_pb12() noexcept override {
        log_event(HwEvent::UnusedGpioAnalog);
    }

    void enter_stop_mode_wfi() noexcept override {
        ++stop_wfi_calls;
        log_event(HwEvent::EnterStopWfi);
    }

    void set_afe_enable(bool enable) noexcept override {
        afe_on = enable;
        if (!enable) {
            log_event(HwEvent::AfeOff);
        }
    }

    bool read_lod_p_high() const noexcept override { return lod_p; }
    bool read_lod_n_high() const noexcept override { return lod_n; }

    void start_ecg_adc_dma(int16_t* dst_490, uint16_t fs_hz) noexcept override {
        dma_dst = dst_490;
        adc_fs_hz = fs_hz;
        adc_dma_running = true;
    }

    void stop_ecg_adc_dma() noexcept override {
        adc_dma_running = false;
        log_event(HwEvent::StopAdcDma);
    }

    void set_bat_sense_enable(bool enable) noexcept override {
        bat_en = enable;
        if (!enable) {
            log_event(HwEvent::BatSenseOff);
        }
    }

    bool read_bat_sense_adc(uint16_t& raw_adc_out) noexcept override {
        if (!bat_adc_ok) {
            return false;
        }
        raw_adc_out = bat_raw;
        return true;
    }

    void set_spi1_pins_analog() noexcept override { log_event(HwEvent::Spi1PinsAnalog); }

    void set_sd_power_enable(bool enable) noexcept override {
        sd_pwr_on = enable;
        if (!enable) {
            log_event(HwEvent::SdPowerOff);
        }
    }

    bool sd_init_card() noexcept override { return sd_init_ok; }

    bool sd_write_sector(uint32_t,
                         const uint8_t* data_512,
                         uint32_t& duration_ms_out) noexcept override {
        ++sd_write_calls;
        duration_ms_out = sd_write_dur_ms;
        if (!sd_write_ok) {
            return false;
        }
        if (sd_stored < sd_sectors.size()) {
            for (std::size_t i = 0U; i < p05::kSdBlockBytes; ++i) {
                sd_sectors[sd_stored][i] = data_512[i];
            }
            ++sd_stored;
        }
        return true;
    }

    p05::Ina219RawTransaction ina219_read() noexcept override { return ina_tx; }

    bool ina219_power_down() noexcept override {
        ++ina_pd_calls;
        log_event(HwEvent::Ina219PowerDown);
        return ina_pd_ok;
    }

    void uart1_send(p05::ConstByteView frame) noexcept override {
        last_uart_len = (frame.length <= last_uart_tx.size()) ? frame.length : last_uart_tx.size();
        for (std::size_t i = 0U; i < last_uart_len; ++i) {
            last_uart_tx[i] = frame.data[i];
        }
    }
};

std::array<uint8_t, 15U> build_hil_config_req(uint16_t seq) noexcept {
    std::array<uint8_t, 15U> b{};
    b[0] = p05::kHilSync0;
    b[1] = p05::kHilSync1;
    b[2] = p05::kHilVersion;
    b[3] = static_cast<uint8_t>(p05::HilMsgType::ConfigReq);
    p05::write_u16_le(&b[4], seq);
    p05::write_u16_le(&b[6], 5U);
    p05::write_u16_le(&b[8], 250U);
    b[10] = 0U;
    b[11] = 1U;
    b[12] = 1U;
    p05::write_u16_le(&b[13], p05::crc16_ccitt_false(b.data(), 13U));
    return b;
}

void test_01_sd_512b_formatter_roundtrip_and_single_bit_flip_rejection() {
    // 1. RAW_ECG_BLOCK round-trip
    p05::TrustedWearableBatch batch{};
    batch.timestamp_ms = 54321U;
    batch.fs_hz = 250U;
    batch.sample_count = 245U;
    for (std::size_t i = 0U; i < 245U; ++i) {
        batch.samples[i] = static_cast<int16_t>(1800 + static_cast<int16_t>(i));
    }

    const auto raw_res = p05::format_sd_raw_ecg_block(42U, 54321U, p05::sd_flags::kFs500Hz, batch);
    EXPECT_TRUE(raw_res.is_ok());
    const auto parsed_raw = p05::parse_sd_raw_ecg_block(
        p05::ConstByteView{raw_res.value().data(), raw_res.value().size()});
    EXPECT_TRUE(parsed_raw.is_ok());
    EXPECT_EQ(parsed_raw.value().header.block_seq, 42U);
    EXPECT_EQ(parsed_raw.value().header.timestamp_ms, 54321U);
    EXPECT_EQ(parsed_raw.value().header.flags, p05::sd_flags::kFs500Hz);
    EXPECT_EQ(parsed_raw.value().valid_sample_count, 245U);
    EXPECT_EQ(parsed_raw.value().samples[0], 1800);
    EXPECT_EQ(parsed_raw.value().samples[244], 2044);

    // Flip 1 bit in header, body, and CRC -> ALL rejected (R-BND-01 / R-SD-02)
    for (const std::size_t pos : {std::size_t{0U}, std::size_t{100U}, std::size_t{509U}, std::size_t{511U}}) {
        p05::SdSectorBuffer corrupted = raw_res.value();
        corrupted[pos] ^= 0x01U;
        const auto r_bad = p05::parse_sd_raw_ecg_block(
            p05::ConstByteView{corrupted.data(), corrupted.size()});
        EXPECT_TRUE(r_bad.is_err());
        EXPECT_EQ(r_bad.canonical_state(), p05::CanonicalState::Malformed);
    }

    // 2. RR_HRV_BLOCK round-trip (valid HRV + battery + INA219)
    p05::SdRrHrvBlockInput rr_in{};
    rr_in.block_seq = 43U;
    rr_in.timestamp_ms = 60000U;
    rr_in.valid_rr_count = 2U;
    rr_in.peaks[0] = p05::TrustedRPeakRecord{1000U, -64, 833000U};
    rr_in.peaks[1] = p05::TrustedRPeakRecord{1208U, 128, 834000U};
    rr_in.hrv_res = p05::Result<p05::TrustedHrvMetrics>::ok(
        p05::TrustedHrvMetrics{833500U, 7199U, 707U, 1000U, 0U});
    rr_in.battery_res =
        p05::Result<p05::TrustedBatteryReading>::ok(p05::TrustedBatteryReading{2450U, 3949U});
    rr_in.ina219_res =
        p05::Result<p05::TrustedIna219Reading>::ok(p05::TrustedIna219Reading{3300U, 7600});

    const auto rr_res = p05::format_sd_rr_hrv_block(rr_in);
    EXPECT_TRUE(rr_res.is_ok());
    const auto parsed_rr = p05::parse_sd_rr_hrv_block(
        p05::ConstByteView{rr_res.value().data(), rr_res.value().size()});
    EXPECT_TRUE(parsed_rr.is_ok());
    EXPECT_EQ(parsed_rr.value().valid_rr_count, 2U);
    EXPECT_EQ(parsed_rr.value().hrv_valid, true);
    EXPECT_EQ(parsed_rr.value().vbat_mv, 3949U);
    EXPECT_EQ(parsed_rr.value().ina219_bus_mv, 3300U);
    EXPECT_EQ(parsed_rr.value().ina219_current_ua_div10, 760);
    EXPECT_EQ(parsed_rr.value().hrv.hr_bpm_x100, 7199U);
    EXPECT_EQ(parsed_rr.value().peaks[1].rr_interval_us, 834000U);

    // Flip 1 bit in RR_HRV_BLOCK -> rejected
    p05::SdSectorBuffer rr_bad = rr_res.value();
    rr_bad[45] ^= 0x10U;
    EXPECT_EQ(p05::parse_sd_rr_hrv_block(p05::ConstByteView{rr_bad.data(), rr_bad.size()})
                  .detail_code(),
              p05::DetailCode::BadCrc);
}

void test_02_ping_pong_isr_and_overflow_detection() {
    p05::PingPongDmaBuffer pp;
    pp.reset();

    // Fill half 0 and half 1 as DMA would
    int16_t* dst = pp.dma_dest_ptr();
    for (std::size_t i = 0U; i < 245U; ++i) {
        dst[i] = 2000;
        dst[245U + i] = 2100;
    }

    // ISR notifies half 0, then half 1 before main polls -> main drains FIFO (half 0 then half 1)
    pp.isr_notify_half_complete(0U, 1000U);
    pp.isr_notify_half_complete(1U, 1980U);

    const auto h0 = pp.main_poll_ready();
    EXPECT_TRUE(h0.ready);
    EXPECT_EQ(h0.half_index, 0U);
    EXPECT_EQ(h0.timestamp_ms, 1000U);
    EXPECT_TRUE(!h0.overflowed);
    EXPECT_EQ(h0.samples[0], 2000);

    const auto h1 = pp.main_poll_ready();
    EXPECT_TRUE(h1.ready);
    EXPECT_EQ(h1.half_index, 1U);
    EXPECT_TRUE(!h1.overflowed);
    EXPECT_EQ(h1.samples[0], 2100);

    // Simulate slow SD write causing DMA to overwrite unread half 0 -> overflow flag set!
    pp.isr_notify_half_complete(0U, 2960U);
    pp.isr_notify_half_complete(1U, 3940U);
    pp.isr_notify_half_complete(0U, 4920U);  // Overwrites unread half 0!

    const auto h_ovf = pp.main_poll_ready();
    EXPECT_TRUE(h_ovf.ready);
    EXPECT_TRUE(h_ovf.overflowed);
}

void test_03_stop_mode_sequence_and_i2c_retry_policy() {
    MockPlatformHw hw;
    p05::P05AppController app(hw);

    // Boot at 3.95 V -> WearableRun @ 250 Hz, PB5 turned back OFF after reading battery
    app.boot(p05::PowerClockProfile::Run8MHzQ15Sleep);
    EXPECT_EQ(app.mode(), p05::AppMode::WearableRun);
    EXPECT_EQ(hw.adc_fs_hz, 250U);
    EXPECT_TRUE(hw.afe_on);
    EXPECT_TRUE(hw.sd_pwr_on);
    EXPECT_TRUE(!hw.bat_en);  // 2N7002 OFF after reading VBAT (0 uA leak!)

    // Deliver one clean Ping-Pong DMA half -> writes valid 512 B RAW_ECG_BLOCK to SD
    for (std::size_t i = 0U; i < 245U; ++i) {
        hw.dma_dst[i] = 2048;
    }
    hw.now_ms = 1980U;
    app.isr_on_dma_half(0U);
    app.step();
    EXPECT_EQ(hw.sd_stored, 1U);
    EXPECT_TRUE(
        p05::parse_sd_raw_ecg_block(p05::ConstByteView{hw.sd_sectors[0].data(), 512U}).is_ok());

    // Idle step in optimized mode calls Sleep WFI
    hw.now_ms = 2000U;
    app.step();
    EXPECT_TRUE(hw.sleep_wfi_calls >= 1U);

    // Test OPEN-9b exact Stop Mode order AND I2C1 power-down retry (fail -> retry 1x -> still sleep!)
    hw.clear_events();
    hw.ina_pd_ok = false;
    hw.ina_pd_calls = 0U;
    app.request_user_stop();
    app.step();

    EXPECT_EQ(app.mode(), p05::AppMode::StopMode);
    EXPECT_TRUE(!hw.afe_on);
    EXPECT_TRUE(!hw.sd_pwr_on);
    EXPECT_TRUE(!hw.bat_en);
    EXPECT_TRUE(!hw.adc_dma_running);
    EXPECT_EQ(hw.ina_pd_calls, 2U);        // Initial attempt + 1 retry (OPEN-9b)
    EXPECT_TRUE(app.ina219_powerdown_fault());
    EXPECT_EQ(hw.stop_wfi_calls, 1U);

    // Verify critical physical ordering:
    // StopAdcDma < Spi1PinsAnalog < SdPowerOff < Ina219PowerDown < AfeOff < BatSenseOff < UnusedGpioAnalog < EnterStopWfi
    const int i_dma = hw.find_event(HwEvent::StopAdcDma);
    const int i_spi = hw.find_event(HwEvent::Spi1PinsAnalog);
    const int i_sd = hw.find_event(HwEvent::SdPowerOff);
    const int i_ina = hw.find_event(HwEvent::Ina219PowerDown);
    const int i_afe = hw.find_event(HwEvent::AfeOff);
    const int i_bat = hw.find_event(HwEvent::BatSenseOff);
    const int i_gpio = hw.find_event(HwEvent::UnusedGpioAnalog);
    const int i_wfi = hw.find_event(HwEvent::EnterStopWfi);
    EXPECT_TRUE(i_dma >= 0 && i_dma < i_spi);
    EXPECT_TRUE(i_spi < i_sd);  // Prevents ESD-diode back-powering into microSD!
    EXPECT_TRUE(i_sd < i_ina);
    EXPECT_TRUE(i_ina < i_afe);
    EXPECT_TRUE(i_afe < i_bat);
    EXPECT_TRUE(i_bat < i_gpio);
    EXPECT_TRUE(i_gpio < i_wfi);
}

void test_04_failsafe_triggers_low_battery_lod_60s_sd_3x_and_r3_lockout() {
    // 1. Boot with low battery (< 3.40 V) immediately enters StopMode (OPEN-8)
    {
        MockPlatformHw hw;
        hw.bat_raw = 2048U;  // ~3.30 V < 3.40 V
        p05::P05AppController app(hw);
        app.boot();
        EXPECT_EQ(app.mode(), p05::AppMode::StopMode);
        EXPECT_EQ(hw.stop_wfi_calls, 1U);
    }

    // 2. 3 consecutive SD write failures (each retried once -> 6 SPI calls) trigger StopMode (OPEN-3 / OPEN-7b)
    {
        MockPlatformHw hw;
        hw.sd_write_ok = false;
        p05::P05AppController app(hw);
        app.boot();
        for (std::size_t i = 0U; i < 490U; ++i) {
            hw.dma_dst[i] = 2048;
        }
        for (uint8_t blk = 0U; blk < 3U; ++blk) {
            hw.now_ms += 980U;
            app.isr_on_dma_half(blk & 1U);
            app.step();
        }
        EXPECT_EQ(hw.sd_write_calls, 6U);  // 3 blocks x 2 attempts (1 retry each)
        EXPECT_EQ(app.mode(), p05::AppMode::StopMode);
    }

    // 3. Continuous 60 s Leads-Off enters StopMode (OPEN-6d)
    {
        MockPlatformHw hw;
        p05::P05AppController app(hw);
        app.boot();
        hw.lod_p = true;
        hw.now_ms = 2000U;
        app.step();
        hw.now_ms = 2050U;  // 50 ms debounce latches Leads-Off
        app.step();
        EXPECT_EQ(app.mode(), p05::AppMode::WearableRun);
        hw.now_ms = 62050U;  // 60 s continuous Leads-Off -> StopMode!
        app.step();
        EXPECT_EQ(app.mode(), p05::AppMode::StopMode);
    }

    // 4. R-BND-03 / OPEN-2: UART1 HIL with attached electrodes latches lockout; comms fault cannot clear it!
    {
        MockPlatformHw hw;
        hw.lod_p = false;
        hw.lod_n = false;
        p05::P05AppController app(hw);
        app.boot();
        const auto cfg = build_hil_config_req(1U);
        app.on_uart1_frame(p05::ConstByteView{cfg.data(), cfg.size()});
        EXPECT_EQ(app.mode(), p05::AppMode::WearableRun);
        EXPECT_TRUE(app.hil_lockout_latched());
        EXPECT_EQ(hw.last_uart_len, 14U);
        EXPECT_EQ(hw.last_uart_tx[10], static_cast<uint8_t>(p05::CanonicalState::ProtectionStatusUnknown));

        app.on_uart1_comms_fault();
        EXPECT_TRUE(app.hil_lockout_latched());
    }

    // 5. Safe HIL entry when both PA1 & PA2 report detached -> isolates SD & AFE, enters HilMode
    {
        MockPlatformHw hw;
        hw.lod_p = true;
        hw.lod_n = true;
        p05::P05AppController app(hw);
        app.boot();
        const auto cfg = build_hil_config_req(1U);
        app.on_uart1_frame(p05::ConstByteView{cfg.data(), cfg.size()});
        EXPECT_EQ(app.mode(), p05::AppMode::HilMode);
        EXPECT_TRUE(!hw.afe_on);
        EXPECT_TRUE(!hw.sd_pwr_on);
        EXPECT_TRUE(!hw.adc_dma_running);
        EXPECT_EQ(hw.last_uart_tx[3], static_cast<uint8_t>(p05::HilMsgType::ResultAck));
    }
}

}  // namespace

int main() {
    test_01_sd_512b_formatter_roundtrip_and_single_bit_flip_rejection();
    test_02_ping_pong_isr_and_overflow_detection();
    test_03_stop_mode_sequence_and_i2c_retry_policy();
    test_04_failsafe_triggers_low_battery_lod_60s_sd_3x_and_r3_lockout();

    std::printf("P05 Task Card 5 Platform Adapter Mock Tests: %d assertions, %d failed\n",
                g_tests_run, g_tests_failed);
    return (g_tests_failed == 0) ? 0 : 1;
}
