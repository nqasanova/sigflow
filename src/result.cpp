#include "sigflow/result.hpp"

namespace sigflow {

std::string_view to_string(Errc code) noexcept {
    switch (code) {
        case Errc::truncated: return "truncated input";
        case Errc::bad_magic: return "bad frame magic";
        case Errc::unsupported_version: return "unsupported protocol version";
        case Errc::crc_mismatch: return "crc mismatch";
        case Errc::payload_too_large: return "payload too large";
        case Errc::field_too_large: return "field too large";
        case Errc::bad_field_length: return "bad field length";
        case Errc::missing_field: return "missing required field";
        case Errc::duplicate_field: return "duplicate field";
        case Errc::invalid_value: return "invalid value";
        case Errc::unexpected_message: return "unexpected message";
        case Errc::io_error: return "i/o error";
    }
    // Not reachable with a valid enum value. Kept outside the switch so the
    // compiler still warns when a new Errc is added without a case.
    return "unknown error";
}

}  // namespace sigflow
