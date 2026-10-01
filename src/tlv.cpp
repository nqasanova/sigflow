#include "sigflow/tlv.hpp"

#include <string>

namespace sigflow {

Result<std::vector<TlvField>> parse_tlv(ByteView payload) {
    std::vector<TlvField> fields;
    ByteReader reader(payload);

    while (!reader.empty()) {
        auto tag = reader.read_be<std::uint8_t>();
        auto length = reader.read_be<std::uint16_t>();
        if (!tag || !length) {
            return make_error(Errc::truncated,
                              "incomplete field header at offset " + std::to_string(reader.position()));
        }
        auto value = reader.read_bytes(*length);
        if (!value) {
            return make_error(Errc::truncated, "field with tag " + std::to_string(*tag) + " claims " +
                                                   std::to_string(*length) + " bytes but only " +
                                                   std::to_string(reader.remaining()) + " are left");
        }
        fields.push_back(TlvField{*tag, *value});
    }
    return fields;
}

Status TlvWriter::put_raw(std::uint8_t tag, ByteView bytes) {
    if (bytes.size() > kMaxFieldSize) {
        return make_error(Errc::field_too_large,
                          "tag " + std::to_string(tag) + " has " + std::to_string(bytes.size()) + " bytes");
    }
    put_header(tag, static_cast<std::uint16_t>(bytes.size()));
    out_.put_bytes(bytes);
    return Status::success();
}

namespace detail {

Error bad_length(std::size_t actual, std::size_t expected) {
    return make_error(Errc::bad_field_length,
                      "field has " + std::to_string(actual) + " bytes, expected " + std::to_string(expected));
}

}  // namespace detail

}  // namespace sigflow
