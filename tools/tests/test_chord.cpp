// SPDX-License-Identifier: GPL-2.0-or-later
// Button chord detector of the overlay (frontend/overlay/chord.h).
#include <cstdio>
#include <SDL3/SDL_gamepad.h>
#include "frontend/overlay/chord.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { ++failures; std::printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

constexpr int L3 = SDL_GAMEPAD_BUTTON_LEFT_STICK;
constexpr int R3 = SDL_GAMEPAD_BUTTON_RIGHT_STICK;
constexpr int A = SDL_GAMEPAD_BUTTON_SOUTH;

int main() {
    {
        Chord chord({L3, R3});
        CHECK(!chord.Press(1, L3));
        CHECK(chord.Press(1, R3));        // both down: fires
        CHECK(!chord.Press(1, R3));       // repeat while held: no
        CHECK(!chord.Release(1, R3));     // L3 still held: chord not released yet
        CHECK(!chord.Press(1, R3));       // L3 never released: not re-armed
        CHECK(!chord.Release(1, L3));
        CHECK(chord.Release(1, R3));      // last button up after firing: released
        CHECK(!chord.Release(1, R3));     // only once
        CHECK(!chord.Press(1, R3));       // re-armed, order does not matter
        CHECK(chord.Press(1, L3));
    }
    {
        Chord chord({L3, R3});
        CHECK(!chord.Press(1, L3));
        CHECK(!chord.Press(2, R3));       // two controllers do not combine
        CHECK(!chord.Press(1, A));
    }
    {
        Chord one({A});
        CHECK(one.Press(7, A));
        Chord three({L3, R3, A});
        CHECK(!three.Press(1, L3));
        CHECK(!three.Press(1, R3));
        CHECK(three.Press(1, A));
    }
    {
        // A controller removed mid-chord is forgotten.
        Chord chord({L3, R3});
        CHECK(!chord.Press(1, L3));
        chord.RemoveDevice(1);
        CHECK(!chord.Press(1, R3));
    }
    {
        std::vector<int> buttons;
        CHECK(ParseChord("l3+r3", buttons) && buttons == std::vector<int>({L3, R3}));
        CHECK(ParseChord("start", buttons) && buttons == std::vector<int>({SDL_GAMEPAD_BUTTON_START}));
        CHECK(ParseChord(" L3 + plus ", buttons) &&
              buttons == std::vector<int>({L3, SDL_GAMEPAD_BUTTON_START}));
        CHECK(!ParseChord("l3+banana", buttons));
        CHECK(!ParseChord("", buttons));
        CHECK(!ParseChord("l3++r3", buttons));
    }
    if (failures) { std::printf("%d failures\n", failures); return 1; }
    std::printf("chord: fire once, re-arm, per device, removal and parsing passed\n");
    return 0;
}
