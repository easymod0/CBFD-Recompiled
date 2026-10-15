// Netplay's handshake hashes (hello.hpp). The ROM and the profile are hashed as for the harness's
// recordings (harness.cpp), so a recording and a match of the same game carry the same ones.

#include "hello.hpp"

#include <cstring>

#include "librecomp/game.hpp"

#include "../conker.hpp"

#define XXH_INLINE_ALL
#include "xxHash/xxhash.h"

conker::netplay::Hello conker::netplay::local_hello() {
    return hello_for_rom(recomp::get_rom());
}

conker::netplay::Hello conker::netplay::hello_for_rom(std::span<const uint8_t> rom) {
    Hello hello;
    hello.build = XXH3_64bits(build_id(), std::strlen(build_id()));
    hello.rom = XXH3_64bits(rom.data(), rom.size());
    hello.profile = conker::netplay_profile::hash();
    hello.mods = conker::netplay_profile::mods_hash();
    return hello;
}
