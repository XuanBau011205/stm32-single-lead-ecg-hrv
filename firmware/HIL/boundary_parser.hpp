#ifndef P05_HIL_BOUNDARY_PARSER_HPP
#define P05_HIL_BOUNDARY_PARSER_HPP

#include <cstddef>
#include <cstdint>

#include "trusted_types.hpp"

namespace p05 {

// Hardware safety snapshot supplied by Platform Adapter to HIL boundary.
struct HilSafetyInputs {
    bool protection_status_known = false;
    bool electrodes_detached = false;  // true only when both PA1 & PA2 report debounced Leads-Off
};

// Stateless wire-level frame validator for UART1 HIL (Host -> MCU).
// Validates strictly in order: length -> sync -> version -> CRC -> endianness -> semantic ranges.
// Rejects bad frames before any payload value is decoded (R-BND-01 / R1).
Result<TrustedHilCommand> parse_hil_frame_stateless(ConstByteView raw_frame,
                                                    uint16_t active_fs_hz) noexcept;

// Stateful HIL boundary adapter enforcing:
// - R-BND-01 (R1): reject bad length/sync/version/CRC/semantic range at boundary
// - R-BND-02 (R2): never convert fault/stale/gap into zero measurements
// - R-BND-03 (R3 / PROPOSED-R3-ECG): latched safety/mode-conflict lockout cannot be cleared by comms fault
// - R-BND-05: explicit uptime-based staleness (2000 ms) and sequence gap/duplicate/replay checks
// - OPEN-4: 3 consecutive malformed frames abort active HIL session
class HilBoundaryValidator {
public:
    HilBoundaryValidator() noexcept = default;

    Result<TrustedHilCommand> validate_incoming_frame(ConstByteView raw_frame,
                                                      uint32_t uptime_ms,
                                                      const HilSafetyInputs& safety) noexcept;

    // Checks UART1 idle timeout (OPEN-3 / OPEN-5: > 2000 ms while session active).
    FailureStatus check_timeout(uint32_t uptime_ms) noexcept;

    // Records a UART1 transport fault. By R-BND-03, comms fault closes the active HIL session
    // but NEVER clears a latched safety/mode-conflict lockout.
    FailureStatus on_comms_fault() noexcept;

    // Clears a latched lockout ONLY when hardware explicitly reports verified safe state
    // (protection status known AND electrodes detached).
    bool try_clear_lockout(const HilSafetyInputs& safety) noexcept;

    bool hil_session_active() const noexcept { return session_active_; }
    bool lockout_latched() const noexcept { return lockout_latched_; }
    uint8_t consecutive_malformed_count() const noexcept { return consecutive_malformed_; }
    uint16_t active_fs_hz() const noexcept { return active_fs_hz_; }

private:
    void close_session_keep_lockout() noexcept;

    bool session_active_ = false;
    bool lockout_latched_ = false;
    DetailCode lockout_detail_ = DetailCode::ProtectionUnknown;
    bool has_last_seq_ = false;
    bool last_seq_was_valid_ = false;
    bool has_expected_sample_idx_ = false;
    uint16_t last_valid_seq_ = 0U;
    uint32_t expected_next_sample_idx_ = 0U;
    uint32_t last_valid_rx_ms_ = 0U;
    uint16_t active_fs_hz_ = kDefaultWearableFsHz;
    uint8_t consecutive_malformed_ = 0U;
};

}  // namespace p05

#endif  // P05_HIL_BOUNDARY_PARSER_HPP
