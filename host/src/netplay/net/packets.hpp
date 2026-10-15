// Netplay's packets: everything peers send each other, and the only code that reads
// what arrives from the network. Each kind of packet has a fixed size and a version, and decode()
// checks every field: anything else is refused whole.
//
// Peers send inputs only: each its own port's pads (the host passes them on to the others), the hashes
// of the game's memory for the desync check, and the handshake. Never files, mods or save states.
//
// The layout, little-endian: "CN", u16 version, u8 kind, then the kind's fields (see packets.cpp).

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>

namespace conker::netplay {
    // Changes whenever any packet's layout or meaning does: peers with another one are refused.
    constexpr uint16_t protocol_version = 4;
    // The game's controller ports.
    constexpr int max_players = 4;
    // The most reads an Inputs packet carries: a peer catching up can send several at once. (Inputs are
    // sent reliably, so none is lost: lockstep can't go on without every one.)
    constexpr int input_window = 8;
    // The largest packet, to size buffers.
    constexpr size_t max_packet_size = 64;

    // One controller read of one pad, as the N64 gives it to the game (the harness's 5 bytes).
    struct Pad {
        uint16_t buttons = 0;
        int8_t stick_x = 0;
        int8_t stick_y = 0;
        uint8_t err_no = 0;
        bool operator==(const Pad&) const = default;
    };
    // The N64's two unused button bits; a pad with them set is refused.
    constexpr uint16_t reserved_buttons = 0x00C0;
    // A pad's err_no: 0, or the N64's "no controller" (CONT_NO_RESPONSE_ERROR).
    constexpr uint8_t err_no_controller = 0x08;
    bool valid(const Pad& pad);

    // Why a peer is refused or a match ends.
    enum class Reason : uint8_t {
        quit = 1,       // the player left
        version,        // another protocol version
        build,          // another build of the game
        rom,            // another ROM
        profile,        // another netplay settings profile
        full,           // every port is taken
        started,        // the match has already started
        protocol,       // a packet that wasn't expected or didn't decode
        desync,         // the game's memory differs between peers
        timeout,        // a peer stopped answering
        mods,           // mods on (texture packs aside): netplay allows none
        key,            // not this match's key (the join code's): nothing else is checked
    };
    constexpr uint8_t last_reason = (uint8_t)Reason::key;
    const char* describe(Reason reason);

    // Joiner to host, first: the match's key, then what it's running. The host checks the key first, and
    // refuses a wrong one before anything else; then it refuses any difference.
    struct Hello {
        uint64_t build = 0;     // hash of the game's build
        uint64_t rom = 0;       // XXH3-64 of the ROM
        uint64_t profile = 0;   // hash of the netplay settings profile
        uint64_t mods = 0;      // hash of the mods on and their settings, texture packs aside (0: none)
        uint64_t key = 0;       // the match's key, from the join code (the host's own: its random key)
        bool operator==(const Hello&) const = default;
    };
    // Host to joiner: accepted, on this port, in a match of this many players.
    struct Welcome {
        uint8_t port = 0;
        uint8_t players = 0;
        bool operator==(const Welcome&) const = default;
    };
    // Host to joiner: not accepted, then the connection is closed.
    struct Refuse {
        Reason reason = Reason::protocol;
        bool operator==(const Refuse&) const = default;
    };
    // One port's pads for reads first_read to first_read + count - 1.
    struct Inputs {
        uint8_t port = 0;
        uint32_t first_read = 0;
        uint8_t count = 0;
        std::array<Pad, input_window> pads{};   // the first `count` are used, the rest are zero
        bool operator==(const Inputs&) const = default;
    };
    // The XXH3-64 of the game's memory as the sender made read `read`.
    struct Hash {
        uint32_t read = 0;
        uint64_t hash = 0;
        bool operator==(const Hash&) const = default;
    };
    // Host to everyone, once every port is taken: the match starts, with this many players, and whether
    // every player sees only their own view of the split screen (the host's rule for the match: 1), or each
    // chooses (0).
    struct Start {
        uint8_t players = 0;
        uint8_t own_view = 0;
        bool operator==(const Start&) const = default;
    };
    // Leaving, and why.
    struct Bye {
        Reason reason = Reason::quit;
        bool operator==(const Bye&) const = default;
    };

    using Packet = std::variant<Hello, Welcome, Refuse, Inputs, Hash, Bye, Start>;

    // Writes `packet` into `out` and returns its size, or 0 if the packet isn't valid (the same checks
    // as decode's), so nothing invalid is ever sent.
    size_t encode(const Packet& packet, std::span<uint8_t, max_packet_size> out);
    // The packet in `data`, or nothing if it isn't exactly one valid packet of this protocol version.
    std::optional<Packet> decode(std::span<const uint8_t> data);
}
