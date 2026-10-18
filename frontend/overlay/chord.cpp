// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <cctype>
#include <string>
#include <SDL3/SDL_gamepad.h>
#include "frontend/overlay/chord.h"

Chord::Chord(std::vector<int> buttons_) : buttons{std::move(buttons_)} {}

bool Chord::Press(int device, int button) {
    if (std::find(buttons.begin(), buttons.end(), button) == buttons.end()) return false;
    DeviceState& state = devices[device];
    state.held.insert(button);
    if (state.fired || state.held.size() != buttons.size()) return false;
    state.fired = true;
    return true;
}

void Chord::Release(int device, int button) {
    const auto it = devices.find(device);
    if (it == devices.end()) return;
    it->second.held.erase(button);
    if (it->second.held.empty()) it->second.fired = false;
}

void Chord::RemoveDevice(int device) {
    devices.erase(device);
}

namespace {
int ButtonFromName(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (name == "l3") return SDL_GAMEPAD_BUTTON_LEFT_STICK;
    if (name == "r3") return SDL_GAMEPAD_BUTTON_RIGHT_STICK;
    if (name == "l") return SDL_GAMEPAD_BUTTON_LEFT_SHOULDER;
    if (name == "r") return SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
    if (name == "plus") return SDL_GAMEPAD_BUTTON_START;
    if (name == "minus") return SDL_GAMEPAD_BUTTON_BACK;
    return SDL_GetGamepadButtonFromString(name.c_str());
}
} // namespace

bool ParseChord(std::string_view text, std::vector<int>& out) {
    out.clear();
    while (true) {
        const size_t plus = text.find('+');
        std::string_view part = text.substr(0, plus);
        while (!part.empty() && std::isspace(static_cast<unsigned char>(part.front()))) part.remove_prefix(1);
        while (!part.empty() && std::isspace(static_cast<unsigned char>(part.back()))) part.remove_suffix(1);
        if (part.empty()) return false;
        const int button = ButtonFromName(std::string(part));
        if (button == SDL_GAMEPAD_BUTTON_INVALID) return false;
        out.push_back(button);
        if (plus == std::string_view::npos) return true;
        text.remove_prefix(plus + 1);
    }
}
