// SPDX-License-Identifier: GPL-2.0-or-later
#include <chrono>
#include "frontend/overlay/overlay.h"

AcreOverlay::AcreOverlay(OverlayConfig config_, std::unique_ptr<GameInput> game_input_)
    : config{std::move(config_)}, game_input{std::move(game_input_)}, chord{config.pad_chord} {
    if (config.start_open) {
        Open();
    }
}

AcreOverlay::~AcreOverlay() = default;

bool AcreOverlay::OnSdlEvent(const SDL_Event& event) {
    const auto queue = [&](const OverlayInput& input) {
        std::scoped_lock lock{mutex};
        pending_input.push_back(input);
    };
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const int scancode = static_cast<int>(event.key.scancode);
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        if (scancode == config.key_scancode) {
            // Toggles on the release, so the key's own up never reaches the game.
            if (down) {
                key_down = true;
            } else if (key_down) {
                key_down = false;
                Toggle();
            }
            return true;
        }
        if (open) {
            queue({OverlayInput::Kind::Key, scancode, down});
            return true;
        }
        if (down) {
            held_keys.insert(scancode);
        } else {
            held_keys.erase(scancode);
        }
        return false;
    }
    case SDL_EVENT_MOUSE_MOTION:
        if (!open) return false;
        queue({OverlayInput::Kind::MouseMove, 0, false, event.motion.x, event.motion.y});
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        const bool down = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        if (!open) {
            mouse_held = down;
            return false;
        }
        queue({OverlayInput::Kind::MouseButton, event.button.button, down, event.button.x,
               event.button.y});
        return true;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (!open) return false;
        queue({OverlayInput::Kind::MouseWheel, 0, false, event.wheel.x, event.wheel.y});
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        const bool down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        const int button = event.gbutton.button;
        if (down) {
            if (chord.Press(static_cast<int>(event.gbutton.which), button)) {
                return true;
            }
        } else if (chord.Release(static_cast<int>(event.gbutton.which), button)) {
            // The chord's buttons are not handed to the overlay's navigation.
            Toggle();
            return true;
        }
        if (open) queue({OverlayInput::Kind::PadButton, button, down});
        return open;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (open) {
            queue({OverlayInput::Kind::PadAxis, event.gaxis.axis, false,
                   static_cast<float>(event.gaxis.value) / 32767.0f});
        }
        return open;
    case SDL_EVENT_GAMEPAD_REMOVED:
        chord.RemoveDevice(static_cast<int>(event.gdevice.which));
        return false;
    default:
        return false;
    }
}

bool AcreOverlay::IsOpen() const {
    return open;
}

void AcreOverlay::Toggle() {
    if (open) {
        Close();
    } else {
        Open();
    }
}

void AcreOverlay::Open() {
    // The game must not keep anything pressed that it will never see released.
    for (const int scancode : held_keys) {
        game_input->ReleaseKey(scancode);
    }
    held_keys.clear();
    if (mouse_held) {
        game_input->ReleaseMouseButtons();
        mouse_held = false;
    }
    game_input->BlockControllers(true);
    open = true;
    std::scoped_lock lock{mutex};
    shown_open = true;
}

void AcreOverlay::Close() {
    game_input->BlockControllers(false);
    open = false;
    std::scoped_lock lock{mutex};
    shown_open = false;
    pending_input.clear();
}

void AcreOverlay::OnFrameDisplayed() {
    frame_stats.OnFrame(static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             std::chrono::steady_clock::now().time_since_epoch())
                                             .count()));
}

const VideoCore::OverlayFontAtlas& AcreOverlay::Font() {
    return ui.Font();
}

VideoCore::OverlayDrawData AcreOverlay::Build(float width, float height) {
    OverlayModel model;
    std::vector<OverlayInput> inputs;
    {
        std::scoped_lock lock{mutex};
        model.open = shown_open;
        inputs.swap(pending_input);
    }
    model.fps_counter = config.fps_counter;
    if (!model.open && !model.fps_counter) {
        return {};
    }
    model.fps = static_cast<float>(frame_stats.Summarize(1'000'000'000).fps);
    ui.SetDisplaySize(width, height);
    for (const auto& input : inputs) {
        ui.QueueInput(input);
    }
    return ui.BuildFrame(model);
}
