// The netplay settings profile (fork issue #5): while the harness records, replays or hashes (and,
// later, in netplay), the settings that change what the game does or what's in its memory are fixed,
// so that two runs on two machines, or in a window and headless, are given the same game. Each setting's
// reader checks conker::netplay_profile::active() and uses the profile's value:
//
//   Off: the mouse and gyro (in R-Look, the aiming modes, the free camera and the bank's leaps), Mouse:
//     Turn the Camera, Right Stick: Free Camera, Aiming: Swap Sticks (look_aim.cpp, mouse_camera.cpp):
//     they move the camera in the game's memory from input the recording doesn't hold, or by the time.
//   At their defaults (as headless, which has no settings): the Camera tab's (Field of View, Invert
//     Turning, Turning Speed, the stick's aiming, Lock-On), Conker's Detail (Always Highest), Cutscene
//     Aspect Ratio (Expand), and the sound volumes (100%, as they're set in the game's synthesizer).
//   Widescreen: the culls and sprites are widened for 16:9, whatever the window's size and the aspect
//     ratio setting (widescreen.cpp). A wider window can lose a little at its sides.
//   Save states (F5, F7): off. Mods: refused, but for texture packs (check_mods).
//
// Left to the player: what only changes the picture or the sound outside the game's memory (the
// renderer's settings, Overscan Borders, Show FPS, the reticle, texture packs), and what only changes
// the pads the game is given (the controls, Z turning the shotgun's aim): the pads are what's recorded
// and what netplay sends.
//
// Its hash, of the text below, goes into recordings (and later the netplay handshake): a change to the
// profile changes the text, and a recording made with another profile is refused.

#include <cstdio>
#include <string>

#include "librecomp/mods.hpp"

#include "../conker.hpp"

#define XXH_INLINE_ALL
#include "xxHash/xxhash.h"

namespace {
    bool on = false;

    constexpr const char* profile_text =
        "conker netplay profile 1\n"
        "mouse off; gyro off; mouse camera off; stick free camera off; swap sticks off\n"
        "camera tab defaults; conker detail always highest; cutscene aspect expand; volumes 100\n"
        "widescreen culls 16:9\n"
        "save states off; mods off but texture packs\n";
}

bool conker::netplay_profile::active() {
    return on;
}

void conker::netplay_profile::set_active(bool active) {
    on = active;
}

uint64_t conker::netplay_profile::hash() {
    return XXH3_64bits(profile_text, std::char_traits<char>::length(profile_text));
}

bool conker::netplay_profile::check_mods() {
    bool ok = true;
    for (const recomp::mods::ModDetails& mod : recomp::mods::get_all_mod_details("conker")) {
#if defined(CONKER_RT64)
        if (conker::texture_packs::is_pack(mod.mod_id)) {
            continue;
        }
#endif
        if (recomp::mods::is_mod_enabled(mod.mod_id)) {
            std::fprintf(stderr, "[netplay] the mod %s is on: turn it off in the Mods menu (only texture packs can be on)\n",
                mod.display_name.c_str());
            ok = false;
        }
    }
    return ok;
}
