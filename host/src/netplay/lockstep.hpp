// Netplay's lockstep (fork issue #8): every peer runs the whole game, and at each controller read gives
// it every player's pad for that read, so the same inputs give each the same game. A peer's own pad is
// sent a few reads ahead (the input delay) to hide the time it takes to arrive; the game waits at a read
// until every pad for it is there. Every so often the peers compare hashes of the game's memory, and a
// difference ends the match (a desync).
//
// Part of the harness (harness.cpp), which gives it the deterministic timeline, the settings profile
// and an empty save on each peer. For now it's started from the command line (the menu is #9):
//
//   --netplay-host <port>        hosts a match on UDP <port> (port 1 of the game is the host's)
//   --players <n>                ...of <n> players, 2 to 4 (2 by default)
//   --netplay-join <code>        joins the host by its join code, or its address ("1.2.3.4:5000")
//   --input-delay <reads>        how many reads ahead a peer's own pads are sent (2 by default, 0 to 8;
//                                every peer must give the same)
//   --netplay-desync-at <read>   (a test) changes a byte of game memory at that read, which the next
//                                hash comparison must catch
//
// The game waits at its start for every player to join. With --replay, each peer's own pads come from
// its port in the recording instead of its controller: peers replaying the same recording give the
// game exactly what a replay alone would (tools/netplay/lockstep.sh tests that).

#pragma once

#include <cstdint>
#include <functional>

#include "ultramodern/input.hpp"

#include "net/packets.hpp"

namespace conker::lockstep {
    // A command line option of lockstep's, taking its value too (true if it was one).
    bool parse_option(int argc, char** argv, int& i);
    // Hosting or joining.
    bool active();
    // Checks the options and starts the networking, before the game starts (false: a message was printed).
    bool prepare();
    // Hosts or joins, and waits until every player is in and the match starts, before any of the game's
    // code runs (false: it couldn't, and a message was printed).
    bool connect(const netplay::Hello& me);
    // This peer's port and the match's players, once connected.
    int port();
    int players();

    // This peer's own pad for read `for_read`: `live` holds the pads the controllers give now, or is null
    // for the reads before the first (sent at the start, as nobody has played yet).
    using PadSource = std::function<netplay::Pad(uint64_t for_read, const ultramodern::input::Pad* live)>;
    // At controller read `read`, on the game thread: sends this peer's pad for read + delay, waits for
    // every player's pad for `read` and puts them in `pads`, and compares the memory hash (`hash` is
    // called for it every so often). False if the match has ended (a message was printed): the game
    // can't go on.
    bool on_read(uint64_t read, ultramodern::input::Pad* pads, const PadSource& source,
        const std::function<uint64_t()>& hash, uint8_t* rdram);
    // Says goodbye (after the last read of a replay): what's been sent still arrives.
    void leave();
    // Says goodbye if not yet, and stops the networking, as the game quits.
    void finish();
}
