// Join codes (fork issue #7): the host's address and port written as a short code to pass on, instead
// of "203.0.113.7:27015". There is no server: the code is the address itself, with a check against
// typos, so anyone with a code can connect just as with the address (it isn't a password).
//
// An IPv4 code has 12 characters ("SC07-21V9-GX40" is 203.0.113.7:27015), an IPv6 one 31. They're
// Crockford's base32 (0-9 and the letters but I, L, O and U), read without regard to case, dashes or
// spaces, with O as 0 and I and L as 1.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace conker::netplay {
    struct Endpoint {
        bool ipv6 = false;
        std::array<uint8_t, 16> ip{};   // an IPv4 address in the first 4 bytes, the rest zero
        uint16_t port = 0;
        bool operator==(const Endpoint&) const = default;
    };

    // The code for `endpoint`, in groups of 4 with dashes; empty if its port is 0 (or an IPv4
    // endpoint's last 12 bytes aren't zero).
    std::string encode_join_code(const Endpoint& endpoint);
    // The endpoint in `code`, or nothing if it isn't one (wrong length or characters, or a typo the
    // check caught).
    std::optional<Endpoint> decode_join_code(std::string_view code);
}
