// SPDX-License-Identifier: GPL-2.0-or-later
// Opening and closing the overlay (frontend/overlay/overlay.h) and what the
// game's input sees meanwhile, with a fake in place of input_common and HID.
#include <cstdio>
#include <string>
#include <vector>
#include "frontend/overlay/overlay.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { ++failures; std::printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

struct Log {
    std::vector<std::string> calls;
};

class FakeGameInput final : public GameInput {
public:
    explicit FakeGameInput(Log& log_) : log{log_} {}
    void ReleaseKey(int scancode) override { log.calls.push_back("release key " + std::to_string(scancode)); }
    void ReleaseMouseButtons() override { log.calls.push_back("release mouse"); }
    void BlockControllers(bool blocked) override { log.calls.push_back(blocked ? "block" : "unblock"); }

private:
    Log& log;
};

static SDL_Event Key(SDL_Scancode scancode, bool down) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.scancode = scancode;
    e.key.down = down;
    return e;
}

static SDL_Event Pad(SDL_JoystickID which, SDL_GamepadButton button, bool down) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
    e.gbutton.which = which;
    e.gbutton.button = static_cast<Uint8>(button);
    e.gbutton.down = down;
    return e;
}

static SDL_Event MouseButton(bool down) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    e.button.button = SDL_BUTTON_LEFT;
    e.button.down = down;
    return e;
}

static OverlayConfig Config() {
    OverlayConfig config;
    config.key_scancode = SDL_SCANCODE_INSERT;
    config.pad_chord = {SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK};
    return config;
}

int main() {
    {
        // Insert toggles on release, and never reaches the game.
        Log log;
        AcreOverlay overlay(Config(), std::make_unique<FakeGameInput>(log));
        CHECK(!overlay.OnSdlEvent(Key(SDL_SCANCODE_W, true)));     // game key, game sees it
        CHECK(overlay.OnSdlEvent(MouseButton(true)) == false);
        CHECK(overlay.OnSdlEvent(Key(SDL_SCANCODE_INSERT, true)));
        CHECK(!overlay.IsOpen());                                   // not on the press
        CHECK(overlay.OnSdlEvent(Key(SDL_SCANCODE_INSERT, false)));
        CHECK(overlay.IsOpen());
        // Opening releases what the game had pressed and blocks controllers.
        CHECK(log.calls == std::vector<std::string>({"release key " + std::to_string(SDL_SCANCODE_W),
                                                      "release mouse", "block"}));
        // While open nothing reaches the game.
        CHECK(overlay.OnSdlEvent(Key(SDL_SCANCODE_A, true)));
        CHECK(overlay.OnSdlEvent(Key(SDL_SCANCODE_W, false)));
        CHECK(overlay.OnSdlEvent(MouseButton(false)));
        CHECK(overlay.OnSdlEvent(Pad(1, SDL_GAMEPAD_BUTTON_SOUTH, true)));
        log.calls.clear();
        CHECK(overlay.OnSdlEvent(Key(SDL_SCANCODE_INSERT, true)));
        CHECK(overlay.OnSdlEvent(Key(SDL_SCANCODE_INSERT, false)));
        CHECK(!overlay.IsOpen());
        CHECK(log.calls == std::vector<std::string>({"unblock"}));
        // A key held through the overlay was never seen pressed by the game;
        // its release is harmless and goes through.
        CHECK(!overlay.OnSdlEvent(Key(SDL_SCANCODE_A, false)));
    }
    {
        // L3+R3 toggles when the chord is released, on one controller.
        Log log;
        AcreOverlay overlay(Config(), std::make_unique<FakeGameInput>(log));
        overlay.OnSdlEvent(Pad(1, SDL_GAMEPAD_BUTTON_LEFT_STICK, true));
        CHECK(overlay.OnSdlEvent(Pad(1, SDL_GAMEPAD_BUTTON_RIGHT_STICK, true)));
        CHECK(!overlay.IsOpen());
        overlay.OnSdlEvent(Pad(1, SDL_GAMEPAD_BUTTON_RIGHT_STICK, false));
        CHECK(!overlay.IsOpen());                                   // L3 still held
        overlay.OnSdlEvent(Pad(1, SDL_GAMEPAD_BUTTON_LEFT_STICK, false));
        CHECK(overlay.IsOpen());
        CHECK(log.calls == std::vector<std::string>({"block"}));
        overlay.OnSdlEvent(Pad(1, SDL_GAMEPAD_BUTTON_LEFT_STICK, true));
        overlay.OnSdlEvent(Pad(1, SDL_GAMEPAD_BUTTON_RIGHT_STICK, true));
        overlay.OnSdlEvent(Pad(1, SDL_GAMEPAD_BUTTON_LEFT_STICK, false));
        overlay.OnSdlEvent(Pad(1, SDL_GAMEPAD_BUTTON_RIGHT_STICK, false));
        CHECK(!overlay.IsOpen());
        CHECK(log.calls.back() == "unblock");
    }
    {
        // A controller unplugged mid-chord does not open the overlay later.
        Log log;
        AcreOverlay overlay(Config(), std::make_unique<FakeGameInput>(log));
        overlay.OnSdlEvent(Pad(1, SDL_GAMEPAD_BUTTON_LEFT_STICK, true));
        SDL_Event removed{};
        removed.type = SDL_EVENT_GAMEPAD_REMOVED;
        removed.gdevice.which = 1;
        overlay.OnSdlEvent(removed);
        overlay.OnSdlEvent(Pad(1, SDL_GAMEPAD_BUTTON_RIGHT_STICK, true));
        overlay.OnSdlEvent(Pad(1, SDL_GAMEPAD_BUTTON_RIGHT_STICK, false));
        CHECK(!overlay.IsOpen());
    }
    {
        // --overlay-open only shows the panel, for measuring its cost with a
        // TAS script driving the game: it takes no input and blocks nothing.
        Log log;
        OverlayConfig config = Config();
        config.start_open = true;
        AcreOverlay overlay(config, std::make_unique<FakeGameInput>(log));
        CHECK(!overlay.IsOpen());
        CHECK(log.calls.empty());
        CHECK(!overlay.OnSdlEvent(Key(SDL_SCANCODE_W, true)));
        overlay.Build(1280, 720);                                   // builds without input
    }
    {
        // Reopening starts the overlay's own input clean: a direction held
        // when it closed does not keep scrolling (the queue starts with a reset).
        Log log;
        AcreOverlay overlay(Config(), std::make_unique<FakeGameInput>(log));
        overlay.OnSdlEvent(Key(SDL_SCANCODE_INSERT, true));
        overlay.OnSdlEvent(Key(SDL_SCANCODE_INSERT, false));
        overlay.OnSdlEvent(Pad(1, SDL_GAMEPAD_BUTTON_DPAD_DOWN, true));
        overlay.OnSdlEvent(Key(SDL_SCANCODE_INSERT, true));
        overlay.OnSdlEvent(Key(SDL_SCANCODE_INSERT, false));
        overlay.OnSdlEvent(Key(SDL_SCANCODE_INSERT, true));
        overlay.OnSdlEvent(Key(SDL_SCANCODE_INSERT, false));
        const auto input = overlay.TakePendingInput();
        CHECK(!input.empty() && input.front().kind == OverlayInput::Kind::Reset);
    }
    {
        // Mouse positions arrive in window points; the overlay works in pixels.
        Log log;
        OverlaySources sources;
        sources.pixel_density = [] { return 2.0f; };
        AcreOverlay overlay(Config(), std::make_unique<FakeGameInput>(log), std::move(sources));
        overlay.OnSdlEvent(Key(SDL_SCANCODE_INSERT, true));
        overlay.OnSdlEvent(Key(SDL_SCANCODE_INSERT, false));
        overlay.TakePendingInput();
        SDL_Event motion{};
        motion.type = SDL_EVENT_MOUSE_MOTION;
        motion.motion.x = 100;
        motion.motion.y = 50;
        overlay.OnSdlEvent(motion);
        const auto input = overlay.TakePendingInput();
        CHECK(input.size() == 1 && input[0].x == 200 && input[0].y == 100);
    }
    if (failures) { std::printf("%d failures\n", failures); return 1; }
    std::printf("overlay input: key, chord, release on open, unplug and start open passed\n");
    return 0;
}
