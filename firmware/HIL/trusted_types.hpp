#ifndef P05_HIL_TRUSTED_TYPES_HPP
#define P05_HIL_TRUSTED_TYPES_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace p05 {

// Protocol and hardware constants approved in Rule/PROTOCOL.md & Rule/Open_And_Proposed.md
inline constexpr uint8_t kHilSync0 = 0xA5U;
inline constexpr uint8_t kHilSync1 = 0x5AU;
inline constexpr uint8_t kHilVersion = 0x01U;

inline constexpr std::size_t kHilHeaderBytes = 8U;
inline constexpr std::size_t kHilCrcBytes = 2U;
inline constexpr std::size_t kHilMinFrameBytes = kHilHeaderBytes + kHilCrcBytes;
inline constexpr std::size_t kMaxBatchSamples = 64U;  // OPEN-14
inline constexpr std::size_t kHilMaxPayloadBytes = 5U + (2U * kMaxBatchSamples);  // 133 B
inline constexpr std::size_t kHilMaxFrameBytes = kHilHeaderBytes + kHilMaxPayloadBytes + kHilCrcBytes;  // 143 B

inline constexpr std::size_t kSdBlockBytes = 512U;
inline constexpr std::size_t kSdHeaderBytes = 16U;
inline constexpr std::size_t kSdBodyBytes = 492U;
inline constexpr std::size_t kSdTrailerBytes = 4U;
inline constexpr std::size_t kSdMaxRawSamples = 245U;  // OPEN-7a / PROPOSED-SD-01
inline constexpr std::size_t kSdMaxRrRecords = 46U;    // OPEN-19 / PROPOSED-SD-02

inline constexpr uint16_t kAdcRawMin = 0U;
inline constexpr uint16_t kAdcRawMax = 4095U;
inline constexpr int16_t kAdcMidpoint = 2048;

inline constexpr uint16_t kFs250Hz = 250U;
inline constexpr uint16_t kFs500Hz = 500U;
inline constexpr uint16_t kDefaultWearableFsHz = kFs250Hz;  // OPEN-25

inline constexpr uint32_t kUartTimeoutMs = 2000U;      // OPEN-3 / OPEN-5
inline constexpr uint32_t kSpiWriteTimeoutMs = 250U;   // OPEN-3
inline constexpr uint32_t kI2cTimeoutMs = 10U;         // OPEN-3
inline constexpr uint32_t kAdcStale250HzMs = 1960U;    // OPEN-5: 2 * (245 / 250) s
inline constexpr uint32_t kAdcStale500HzMs = 980U;     // OPEN-5: 2 * (245 / 500) s
inline constexpr uint32_t kLodDebounceMs = 50U;        // OPEN-6
inline constexpr uint32_t kLodStopTimeoutMs = 60000U;  // OPEN-6
inline constexpr uint8_t kMaxConsecutiveMalformed = 3U;  // OPEN-4
inline constexpr uint8_t kMaxConsecutiveSdFaults = 3U;   // OPEN-7b

inline constexpr uint16_t kVbatLowStopMv = 3400U;      // OPEN-8
inline constexpr uint16_t kVbatRecoverMv = 3600U;      // OPEN-8

// Six canonical failure states mandated by Rule/firmware_rules_capstone.md section 4
enum class CanonicalState : uint8_t {
    Valid = 0x00U,
    SensorUnavailable = 0x01U,
    Malformed = 0x02U,
    Stale = 0x03U,
    CommsFault = 0x04U,
    ProtectionStatusUnknown = 0x05U,
};

// Diagnostic sub-codes approved in Rule/PROTOCOL.md section 2.5 (PROPOSED-STATES-01)
enum class DetailCode : uint8_t {
    Ok = 0x00U,
    DuplicateIgnored = 0x01U,

    LeadsOff = 0x10U,
    AfeShutdown = 0x11U,
    LowBattery = 0x12U,

    BadLength = 0x20U,
    BadSync = 0x21U,
    BadVersion = 0x22U,
    BadCrc = 0x23U,
    SemanticRange = 0x24U,
    SeqReplay = 0x25U,

    StaleTimeout = 0x30U,
    SeqGap = 0x31U,

    UartFault = 0x40U,
    SpiSdFault = 0x41U,
    I2cIna219Fault = 0x42U,
    PingPongOverflow = 0x43U,

    ProtectionUnknown = 0x50U,
    ModeConflictLockout = 0x51U,
};

struct FailureStatus {
    CanonicalState canonical = CanonicalState::ProtectionStatusUnknown;
    DetailCode detail = DetailCode::ProtectionUnknown;

    constexpr bool operator==(const FailureStatus& other) const noexcept {
        return canonical == other.canonical && detail == other.detail;
    }

    constexpr bool operator!=(const FailureStatus& other) const noexcept {
        return !(*this == other);
    }
};

// Result<T> enforces mutual exclusion between a trusted value and a failure state.
// Construct only via ok(...) or fail(...).
template <typename T>
class Result {
public:
    static constexpr Result ok(T value, DetailCode detail = DetailCode::Ok) noexcept {
        const DetailCode safe_detail =
            (detail == DetailCode::DuplicateIgnored) ? DetailCode::DuplicateIgnored : DetailCode::Ok;
        return Result(std::move(value), FailureStatus{CanonicalState::Valid, safe_detail});
    }

    static constexpr Result fail(CanonicalState state, DetailCode detail) noexcept {
        const CanonicalState safe_state =
            (state == CanonicalState::Valid) ? CanonicalState::ProtectionStatusUnknown : state;
        const DetailCode safe_detail =
            (state == CanonicalState::Valid) ? DetailCode::ProtectionUnknown : detail;
        return Result(std::nullopt, FailureStatus{safe_state, safe_detail});
    }

    static constexpr Result fail(FailureStatus status) noexcept {
        return fail(status.canonical, status.detail);
    }

    constexpr bool is_ok() const noexcept {
        return status_.canonical == CanonicalState::Valid && value_.has_value();
    }

    constexpr bool is_err() const noexcept { return !is_ok(); }

    constexpr CanonicalState canonical_state() const noexcept { return status_.canonical; }

    constexpr DetailCode detail_code() const noexcept { return status_.detail; }

    constexpr FailureStatus status() const noexcept { return status_; }

    constexpr const std::optional<T>& optional_value() const noexcept { return value_; }

    constexpr const T& value() const noexcept { return *value_; }

private:
    constexpr Result(std::optional<T> val, FailureStatus st) noexcept
        : value_(std::move(val)), status_(st) {}

    std::optional<T> value_;
    FailureStatus status_;
};

// Explicit non-owning byte view for C++17 (no C++20 std::span).
// Owner: caller retains ownership of [data, data + length) for the duration of the call.
// Nullability: data may be nullptr only when length == 0; boundary parsers reject nullptr.
struct ConstByteView {
    const uint8_t* data = nullptr;
    std::size_t length = 0U;

    constexpr bool valid_pointer() const noexcept { return data != nullptr; }
};

enum class HilMsgType : uint8_t {
    ConfigReq = 0x01U,
    SamplesIn = 0x02U,
    ResultAck = 0x81U,
    ErrorNack = 0x8FU,
};

enum class MathMode : uint8_t {
    Q15 = 0U,
    SoftFloat = 1U,
};

struct TrustedHilConfig {
    uint16_t seq = 0U;
    uint16_t fs_hz = kDefaultWearableFsHz;
    MathMode math_mode = MathMode::Q15;
    bool interp_enable = true;
    bool reset_state = false;
};

struct TrustedSampleBatch {
    uint16_t seq = 0U;
    uint32_t first_sample_idx = 0U;
    uint16_t fs_hz = kDefaultWearableFsHz;
    uint8_t sample_count = 0U;
    std::array<int16_t, kMaxBatchSamples> samples{};
};

struct TrustedHilCommand {
    HilMsgType msg_type = HilMsgType::ConfigReq;
    uint16_t seq = 0U;
    TrustedHilConfig config{};
    TrustedSampleBatch batch{};
};

struct TrustedWearableBatch {
    uint32_t timestamp_ms = 0U;
    uint16_t fs_hz = kDefaultWearableFsHz;
    uint16_t sample_count = 0U;
    std::array<int16_t, kSdMaxRawSamples> samples{};
};

struct TrustedBatteryReading {
    uint16_t raw_adc = 0U;
    uint16_t vbat_mv = 0U;
};

struct TrustedIna219Reading {
    uint16_t bus_mv = 0U;
    int32_t current_ua = 0;
};

struct TrustedRPeakRecord {
    uint32_t peak_sample_idx = 0U;
    int16_t sub_sample_offset_q15 = 0;
    uint32_t rr_interval_us = 0U;
};

struct TrustedHrvMetrics {
    uint32_t mean_rr_us = 0U;
    uint16_t hr_bpm_x100 = 0U;
    uint32_t sdnn_us = 0U;
    uint32_t rmssd_us = 0U;
    uint16_t pnn50_permille = 0U;
};

enum class SdBlockType : uint8_t {
    RawEcg = 0x01U,
    RrHrv = 0x02U,
};

struct TrustedSdHeader {
    uint8_t version = 0U;
    SdBlockType block_type = SdBlockType::RawEcg;
    uint16_t flags = 0U;
    uint32_t block_seq = 0U;
    uint32_t timestamp_ms = 0U;
};

struct TrustedSdRawBlock {
    TrustedSdHeader header{};
    uint16_t valid_sample_count = 0U;
    std::array<int16_t, kSdMaxRawSamples> samples{};
};

struct TrustedSdRrHrvBlock {
    TrustedSdHeader header{};
    uint8_t valid_rr_count = 0U;
    bool hrv_valid = false;
    uint16_t vbat_mv = 0U;
    int16_t ina219_current_ua_div10 = 0;
    uint16_t ina219_bus_mv = 0U;
    TrustedHrvMetrics hrv{};
    std::array<TrustedRPeakRecord, kSdMaxRrRecords> peaks{};
};

// Explicit Little-Endian wire decoding helpers (no reinterpret_cast, RULES section 4).
constexpr uint16_t read_u16_le(const uint8_t* ptr) noexcept {
    return static_cast<uint16_t>(static_cast<uint16_t>(ptr[0]) |
                                 static_cast<uint16_t>(static_cast<uint16_t>(ptr[1]) << 8U));
}

constexpr int16_t read_i16_le(const uint8_t* ptr) noexcept {
    return static_cast<int16_t>(read_u16_le(ptr));
}

constexpr uint32_t read_u32_le(const uint8_t* ptr) noexcept {
    return static_cast<uint32_t>(ptr[0]) | (static_cast<uint32_t>(ptr[1]) << 8U) |
           (static_cast<uint32_t>(ptr[2]) << 16U) | (static_cast<uint32_t>(ptr[3]) << 24U);
}

constexpr void write_u16_le(uint8_t* ptr, uint16_t val) noexcept {
    ptr[0] = static_cast<uint8_t>(val & 0xFFU);
    ptr[1] = static_cast<uint8_t>((val >> 8U) & 0xFFU);
}

constexpr void write_i16_le(uint8_t* ptr, int16_t val) noexcept {
    write_u16_le(ptr, static_cast<uint16_t>(val));
}

constexpr void write_u32_le(uint8_t* ptr, uint32_t val) noexcept {
    ptr[0] = static_cast<uint8_t>(val & 0xFFU);
    ptr[1] = static_cast<uint8_t>((val >> 8U) & 0xFFU);
    ptr[2] = static_cast<uint8_t>((val >> 16U) & 0xFFU);
    ptr[3] = static_cast<uint8_t>((val >> 24U) & 0xFFU);
}

// CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, refIn=false, refOut=false, xorOut=0x0000 (OPEN-13).
// Self-check vector: "123456789" -> 0x29B1.
inline uint16_t crc16_ccitt_false(const uint8_t* data, std::size_t length) noexcept {
    if (data == nullptr) {
        return 0xFFFFU;
    }
    uint16_t crc = 0xFFFFU;
    for (std::size_t i = 0U; i < length; ++i) {
        crc = static_cast<uint16_t>(crc ^ static_cast<uint16_t>(static_cast<uint16_t>(data[i]) << 8U));
        for (uint8_t bit = 0U; bit < 8U; ++bit) {
            if ((crc & 0x8000U) != 0U) {
                crc = static_cast<uint16_t>((static_cast<uint16_t>(crc << 1U)) ^ 0x1021U);
            } else {
                crc = static_cast<uint16_t>(crc << 1U);
            }
        }
    }
    return crc;
}

}  // namespace p05

#endif  // P05_HIL_TRUSTED_TYPES_HPP
