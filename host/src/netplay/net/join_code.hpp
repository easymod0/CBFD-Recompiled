// Join codes: what a player needs to join a match, as a code to pass on. There is no server: the code
// is the host's address and port, and the match's key (64 random bits the host makes for each match),
// with a check against typos. Without the key a joiner is refused, so only those given the code can take
// a place in the match; but the code is the host's address too, so it's for people the host trusts.
//
// An IPv4 code has 24 characters, an IPv6 one 44. They're Crockford's base32 (0-9 and the letters but
// I, L, O and U), read without regard to case, dashes or spaces, with O as 0 and I and L as 1.

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
        uint64_t key = 0;               // the match's key
        bool operator==(const Endpoint&) const = default;
    };

    // The code for `endpoint`, in groups of 4 with dashes; empty if its port is 0 (or an IPv4
    // endpoint's last 12 bytes aren't zero).
    std::string encode_join_code(const Endpoint& endpoint);
    // The endpoint in `code`, or nothing if it isn't one (wrong length or characters, or a typo the
    // check caught).
    std::optional<Endpoint> decode_join_code(std::string_view code);
}
