// Netplay's "my view only": in a match, each machine still runs the whole game, split
// screen and all (every player's view is drawn on every machine, so their games stay the same), but the
// window can show only this player's view, across the whole window. RT64 draws that view anew across the whole
// frame with its projection made taller to match, so it isn't stretched and shows more of the scene, and leaves
// the other views out (rt64.patch, RT64::setViewFocusRedraw; widescreen then widens it as any full-frame view).
// Only the renderer changes, not game memory, so it can't desync. It's on when the player chooses it, or when
// the host's rule for the match is that everyone sees only their own view (lockstep.hpp: the default). That
// rule can't be enforced against a modified build (every machine draws every view); it keeps honest players
// on the same footing.
//
// While the game is paused its menu is drawn over the whole split screen, so the picture is left as the game
// draws it (the pause counter D_80082F90, widescreen.cpp).
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
    // rt64.patch: one view of the split screen drawn anew across the whole frame, the others not drawn
    // (rectangles in pixels of the frame).
    void setViewFocusRedraw(bool on, float left, float top, float right, float bottom, float frameWidth, float frameHeight);
}

namespace {
    constexpr gpr cameras_pointer = (gpr)(int32_t)0x800BE628;
    constexpr gpr frame_width_address = (gpr)(int32_t)0x800BE620;
    constexpr gpr pause_counter_address = (gpr)(int32_t)0x80082F90;
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

    // Draws the part of the frame given (shares of it) across the whole frame; (0, 0, 1, 1) draws it all as it is.
    void show(float left, float top, float right, float bottom, int32_t frame_width) {
        const float next[4] = { left, top, right, bottom };
        const bool whole = left <= 0.0f && top <= 0.0f && right >= 1.0f && bottom >= 1.0f;
        const float w = (float)frame_width;
        RT64::setViewFocusRedraw(!whole, left * w, top * frame_height, right * w, bottom * frame_height, w, frame_height);
        if (std::memcmp(next, shown, sizeof(shown)) != 0) {
            std::memcpy(shown, next, sizeof(shown));
            std::printf("[netplay] drawing %.3f %.3f %.3f %.3f of the picture across it\n", left, top, right, bottom);
        }
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
    // The player's choice, or the match's rule (the host's) that everyone sees only their own view.
    const bool wanted = mine_only || conker::lockstep::own_view_for_everyone();
    if (!wanted || !(in_match || conker::harness::replaying()) || port < 0 || cameras < 0x80000000u || cameras >= 0x80800000u ||
        frame_width < 16 || frame_width > 640) {
        show(0, 0, 1, 1, frame_width);
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
    // Paused: the pause menu is drawn over the whole split screen, so it's shown as the game draws it.
    // (D_80082F90 counts the frames since the pause, widescreen.cpp; 0 while not paused.)
    const uint32_t pause_counter = (uint32_t)MEM_W(0, pause_counter_address);
    if (pause_counter != 0) {
        show(0, 0, 1, 1, frame_width);
        return;
    }
    float first[4];
    rect(0, first);
    // Player 1's view reaches every side (to within the 2-pixel border): not split.
    const bool split = !(first[0] < 0.03f && first[1] < 0.03f && first[2] > 0.97f && first[3] > 0.97f);
    if (!split) {
        show(0, 0, 1, 1, frame_width);
        return;
    }
    float own[4];
    rect(port, own);
    if (own[2] - own[0] < 0.05f || own[3] - own[1] < 0.05f) {
        show(0, 0, 1, 1, frame_width);
        return;
    }
    show(own[0], own[1], own[2], own[3], frame_width);
}
