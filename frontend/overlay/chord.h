// SPDX-License-Identifier: GPL-2.0-or-later
// A combination of gamepad buttons pressed together on one controller, such as
// L3+R3 for the overlay. Buttons are SDL_GamepadButton values; devices are SDL
// joystick instance ids.
#pragma once

#include <map>
#include <set>
#include <string_view>
#include <vector>

class Chord {
public:
    explicit Chord(std::vector<int> buttons);
    /// True once, when this press completes the chord on `device`. It fires
    /// again only after every chord button on that device has been released.
    bool Press(int device, int button);
    void Release(int device, int button);
    /// Forgets a disconnected controller.
    void RemoveDevice(int device);

private:
    struct DeviceState {
        std::set<int> held;
        bool fired = false;
    };
    std::vector<int> buttons;
    std::map<int, DeviceState> devices;
};

/// Parses "l3+r3": SDL gamepad button names, plus the aliases l3, r3, l, r,
/// plus and minus, separated by '+'. False, leaving `out` unspecified, on an
/// empty or unknown name.
bool ParseChord(std::string_view text, std::vector<int>& out);
