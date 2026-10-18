// SPDX-License-Identifier: GPL-2.0-or-later
// The in-game overlay as the rest of the frontend sees it: it looks at every
// SDL event first (main thread), opens and closes on its key or button chord,
// keeps game input away while open, and builds its frames for the renderer
// (GPU thread).
#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <vector>
#include <SDL3/SDL_events.h>
#include "frontend/acre_ini.h"
#include "frontend/live_settings.h"
#include "frontend/overlay/chord.h"
#include "frontend/overlay/frame_stats.h"
#include "frontend/overlay/overlay_ui.h"
#include "video_core/frame_overlay.h"

/// What the overlay may do to the game's input; the real one talks to
/// input_common and the HID core, tests use a fake.
class GameInput {
public:
    virtual ~GameInput() = default;
    /// Tells the game a keyboard key (SDL scancode) is up.
    virtual void ReleaseKey(int scancode) = 0;
    /// Tells the game every mouse button is up.
    virtual void ReleaseMouseButtons() = 0;
    /// While blocked the game reads every controller as idle.
    virtual void BlockControllers(bool blocked) = 0;
};

/// Where the overlay's live data and settings go, filled in by the window.
struct OverlaySources {
    std::function<OverlayAotView()> aot;
    std::function<std::vector<std::string>(int min_level)> log_lines;
    LiveTargets live;                       // called on the main thread
};

struct OverlayConfig {
    int key_scancode = 0;            // SDL_Scancode that toggles the overlay
    std::vector<int> pad_chord;      // SDL_GamepadButton values
    bool fps_counter = false;
    bool start_open = false;         // --overlay-open, for measuring
    AcreIni settings;                // as loaded at start-up
    std::filesystem::path save_path; // where "Save to ini" writes
    std::string ini_template;        // used when that file does not exist yet
};

class AcreOverlay final : public VideoCore::FrameOverlay {
public:
    AcreOverlay(OverlayConfig config, std::unique_ptr<GameInput> game_input,
                OverlaySources sources = {});
    ~AcreOverlay() override;

    /// Main thread. True when the event must not reach the game.
    bool OnSdlEvent(const SDL_Event& event);
    bool IsOpen() const;
    /// GPU thread, once per presented frame.
    void OnFrameDisplayed();
    /// Main thread: applies settings changed in the overlay and saves the ini.
    void RunMainActions();

    const VideoCore::OverlayFontAtlas& Font() override;
    VideoCore::OverlayDrawData Build(float width, float height) override;

private:
    void Toggle();
    void Open();
    void Close();

    OverlayConfig config;
    std::unique_ptr<GameInput> game_input;
    Chord chord;

    // Main thread.
    bool open = false;
    bool key_down = false;          // toggle key pressed, waiting for its release
    bool chord_fired = false;       // chord completed, waiting for its release
    std::set<int> held_keys;        // keys the game has seen pressed
    bool mouse_held = false;

    // Shared between the main and GPU threads.
    mutable std::mutex mutex;
    bool shown_open = false;
    std::vector<OverlayInput> pending_input;

    std::vector<IniChange> main_changes;    // to apply on the main thread
    bool main_save = false;
    std::string save_message;

    // Main thread.
    std::vector<IniChange> unsaved;
    OverlaySources sources;

    // GPU thread.
    OverlayUi ui;
    FrameStats frame_stats;
    ThreadCpu thread_cpu;
    AcreIni settings;
    bool fps_counter = false;
    u64 last_refresh_ns = 0;
    std::vector<ThreadLoad> threads;
    OverlayAotView aot;
    std::vector<std::string> log_lines;
};
