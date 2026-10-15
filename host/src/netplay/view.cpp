// Netplay's "my view only" (fork issue #9): in a match, each machine still runs the whole game, split
// screen and all (every player's view is drawn on every machine, so their games stay the same), but the
// window can show only this player's view, scaled up. It's done as the picture is shown (rt64.patch,
// RT64::setPresentationFocus), so nothing in game memory changes and it can't desync.
//
// Each frame, the game's cameras (D_800BE628, 0x180 bytes each, the same as reticle.cpp's and
// widescreen.cpp's) say where each player's view is on the N64's screen: + 0x24 top, + 0x28 bottom,
// + 0x2C left, + 0x30 right, floats, on the frame (D_800BE620 wide, 216 high). While player 1's camera
// isn't full screen the split is on, and this machine's port's camera is the part shown.

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "recomp.h"

#include "../conker.hpp"
#include "lockstep.hpp"

namespace RT64 {
    // rt64.patch: the part of the picture to show alone, as shares of it.
    void setPresentationFocus(float left, float top, float right, float bottom);
}

namespace {
    constexpr gpr cameras_pointer = (gpr)(int32_t)0x800BE628;
    constexpr gpr frame_width_address = (gpr)(int32_t)0x800BE620;
    constexpr uint32_t camera_size = 0x180;
    constexpr int32_t camera_top = 0x24, camera_bottom = 0x28, camera_left = 0x2C, camera_right = 0x30;
    constexpr float frame_height = 216.0f;

    std::atomic<bool> mine_only = false;
    float shown[4] = { 0, 0, 1, 1 };

    float read_float(uint8_t* rdram, gpr address) {
        const uint32_t bits = (uint32_t)MEM_W(0, address);
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    void show(float left, float top, float right, float bottom) {
        const float next[4] = { left, top, right, bottom };
        if (std::memcmp(next, shown, sizeof(shown)) == 0) {
            return;
        }
        std::memcpy(shown, next, sizeof(shown));
        RT64::setPresentationFocus(left, top, right, bottom);
        std::printf("[netplay] showing %.3f %.3f %.3f %.3f of the picture\n", left, top, right, bottom);
    }
}

void conker::netplay_view::set_mine_only(bool on) {
    mine_only = on;
}

bool conker::netplay_view::is_mine_only() {
    return mine_only;
}

void conker::netplay_view::game_frame(uint8_t* rdram) {
    // In a match, this machine's port; replaying a recording (to try it), player 1's.
    const bool in_match = conker::lockstep::connected();
    const int port = in_match ? conker::lockstep::port() : 0;
    const uint32_t cameras = (uint32_t)MEM_W(0, cameras_pointer);
    const int32_t frame_width = MEM_W(0, frame_width_address);
    if (!mine_only || !(in_match || conker::harness::replaying()) || port < 0 || cameras < 0x80000000u || cameras >= 0x80800000u ||
        frame_width < 16 || frame_width > 640) {
        show(0, 0, 1, 1);
        return;
    }
    auto rect = [&](int index, float out[4]) {
        const gpr camera = (gpr)(int32_t)(cameras + index * camera_size);
        out[0] = read_float(rdram, camera + camera_left) / (float)frame_width;
        out[1] = read_float(rdram, camera + camera_top) / frame_height;
        out[2] = read_float(rdram, camera + camera_right) / (float)frame_width;
        out[3] = read_float(rdram, camera + camera_bottom) / frame_height;
        for (int i = 0; i < 4; i++) {
            if (!std::isfinite(out[i])) out[i] = 0;
        }
    };
    float first[4];
    rect(0, first);
    // Player 1's view reaches every side (to within the 2-pixel border): not split.
    const bool split = !(first[0] < 0.03f && first[1] < 0.03f && first[2] > 0.97f && first[3] > 0.97f);
    if (!split) {
        show(0, 0, 1, 1);
        return;
    }
    float own[4];
    rect(port, own);
    if (own[2] - own[0] < 0.05f || own[3] - own[1] < 0.05f) {
        show(0, 0, 1, 1);
        return;
    }
    show(own[0], own[1], own[2], own[3]);
}
