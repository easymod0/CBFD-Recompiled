// Achievements: the pop-up as one unlocks, in the bottom-right corner, as other achievement systems show
// theirs: a panel with the achievement's picture, "Achievement unlocked" and its name and description. It
// slides in from the right as it fades in, stays a few seconds and slides back out; unlocks that come
// together are shown one after another. The pictures will be the ones uploaded with each achievement (the
// achievement site's API, to come): until then each has the same placeholder, a medal.
//
// Like the notices (notices.cpp), it's a recompui context of its own that takes no input and no mouse, shown
// only once the game has started (at the launcher it kept the screen black) and made once the launcher's init
// has run (on_ui_ready). show is called from any thread (the game's, as an unlock is seen); update runs on the
// main thread (update_gfx) and moves the panel each frame.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>

#include "recompui/recompui.h"
#include "elements/ui_label.h"
#include "ultramodern/ultramodern.hpp"

#include "conker.hpp"

namespace {
    using clock = std::chrono::steady_clock;
    // Sliding in, staying and sliding out.
    constexpr float slide_seconds = 0.35f;
    constexpr float stay_seconds = 5.0f;
    // How far right of its place it slides from, and its size and margins, in dp.
    constexpr float slide_distance = 140.0f;
    constexpr float panel_width = 520.0f;
    constexpr float margin = 24.0f;
    constexpr float icon_size = 64.0f;

    struct Unlock {
        std::string title;
        std::string description;
    };

    std::mutex waiting_mutex;
    std::deque<Unlock> waiting;

    std::atomic<bool> ui_ready = false;
    bool created = false;
    recompui::ContextId context = recompui::ContextId::null();
    recompui::Element* panel = nullptr;
    recompui::Label* title_label = nullptr;
    recompui::Label* description_label = nullptr;
    bool showing = false;
    clock::time_point shown_at;

    // A round element of the size given, centred in its parent (a share of it).
    recompui::Element* add_circle(recompui::Element* parent, float share, recompui::theme::color color) {
        recompui::Element* circle = context.create_element<recompui::Element>(parent);
        circle->set_position(recompui::Position::Absolute);
        circle->set_left((1.0f - share) * 50.0f, recompui::Unit::Percent);
        circle->set_top((1.0f - share) * 50.0f, recompui::Unit::Percent);
        circle->set_width(share * 100.0f, recompui::Unit::Percent);
        circle->set_height(share * 100.0f, recompui::Unit::Percent);
        // RmlUi takes no percentage for a radius; one past the size is scaled down to fit, a circle.
        circle->set_border_radius(4096.0f, recompui::Unit::Px);
        circle->set_background_color(color);
        return circle;
    }

    void create() {
        context = recompui::create_context();
        context.open();
        context.set_captures_input(false);
        context.set_captures_mouse(false);

        panel = context.create_element<recompui::Element>(context.get_root_element());
        panel->set_position(recompui::Position::Absolute);
        panel->set_right(margin);
        panel->set_bottom(margin);
        panel->set_width(panel_width);
        panel->set_display(recompui::Display::Flex);
        panel->set_flex_direction(recompui::FlexDirection::Row);
        panel->set_align_items(recompui::AlignItems::Center);
        panel->set_gap(14.0f);
        panel->set_padding(12.0f);
        panel->set_border_radius(recompui::theme::border::radius_sm);
        panel->set_border_width(recompui::theme::border::width);
        panel->set_border_color(recompui::theme::color::WarningA50);
        panel->set_background_color(recompui::theme::color::Background2);

        // The picture: a placeholder medal (a gold disc, a darker ring, a gold middle) on a dark tile,
        // until the achievements' own pictures come.
        recompui::Element* icon = context.create_element<recompui::Element>(panel);
        icon->set_position(recompui::Position::Relative);
        icon->set_width(icon_size);
        icon->set_height(icon_size);
        icon->set_flex_shrink(0.0f);
        icon->set_border_radius(recompui::theme::border::radius_sm);
        icon->set_background_color(recompui::theme::color::BGShadow);
        add_circle(icon, 0.72f, recompui::theme::color::Warning);
        add_circle(icon, 0.52f, recompui::theme::color::WarningD);
        add_circle(icon, 0.38f, recompui::theme::color::WarningL);

        recompui::Element* text = context.create_element<recompui::Element>(panel);
        text->set_display(recompui::Display::Flex);
        text->set_flex_direction(recompui::FlexDirection::Column);
        text->set_gap(2.0f);
        text->set_flex_grow(1.0f);
        recompui::Label* heading = context.create_element<recompui::Label>(text, "ACHIEVEMENT UNLOCKED", recompui::theme::Typography::LabelXS);
        heading->set_color(recompui::theme::color::Warning);
        title_label = context.create_element<recompui::Label>(text, "", recompui::theme::Typography::LabelMD);
        title_label->set_color(recompui::theme::color::Text);
        description_label = context.create_element<recompui::Label>(text, "", recompui::theme::Typography::LabelSM);
        description_label->set_color(recompui::theme::color::TextDim);

        context.close();
        created = true;
    }

    float ease_out(float t) {
        t = std::clamp(t, 0.0f, 1.0f);
        return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
    }
}

void conker::achievement_popup::show(const std::string& title, const std::string& description) {
    std::printf("[achievements] unlocked: %s\n", title.c_str());
    std::fflush(stdout);
    std::lock_guard lock{waiting_mutex};
    waiting.push_back({ title, description });
}

void conker::achievement_popup::on_ui_ready() {
    ui_ready = true;
}

void conker::achievement_popup::update() {
    if (!ui_ready || !ultramodern::is_game_started()) {
        return;
    }
    const clock::time_point now = clock::now();
    float elapsed = showing ? std::chrono::duration<float>(now - shown_at).count() : 0.0f;
    if (showing && elapsed >= 2.0f * slide_seconds + stay_seconds) {
        showing = false;
    }
    if (!showing) {
        Unlock next;
        {
            std::lock_guard lock{waiting_mutex};
            if (waiting.empty()) {
                if (created && recompui::is_context_shown(context)) {
                    recompui::hide_context(context);
                }
                return;
            }
            next = waiting.front();
            waiting.pop_front();
        }
        if (!created) {
            create();
        }
        context.open();
        title_label->set_text(next.title);
        description_label->set_text(next.description);
        context.close();
        showing = true;
        shown_at = now;
        elapsed = 0.0f;
    }
    // In, staying, out: how far along it is (1 where it rests), sliding and fading with it.
    float place;
    if (elapsed < slide_seconds) {
        place = ease_out(elapsed / slide_seconds);
    }
    else if (elapsed < slide_seconds + stay_seconds) {
        place = 1.0f;
    }
    else {
        place = 1.0f - ease_out((elapsed - slide_seconds - stay_seconds) / slide_seconds);
    }
    context.open();
    panel->set_translate_2D((1.0f - place) * slide_distance, 0.0f);
    panel->set_opacity(place);
    context.close();
    if (!recompui::is_context_shown(context)) {
        recompui::show_context(context, "");
    }
}
