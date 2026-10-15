// SPDX-License-Identifier: GPL-2.0-or-later
// Scripted input for repeatable benchmarks. Plays a TAS script (suyu's TAS-nx
// format, script0-1.txt in the profile's tas directory) on player 1 from the
// first displayed frame, or records player 1 into one, frame by frame. A script advances one line per
// frame the game presents, so it follows the game rather than the wall clock.

#pragma once

namespace Core {
class System;
}
namespace InputCommon {
class InputSubsystem;
}

enum class AcreTasMode { Off, Play, Record };

/// Before the input subsystem starts (the SDL window starts it).
void AcreTasConfigure(AcreTasMode mode);

/// Once the window exists: starts playback or recording.
void AcreTasStart(InputCommon::InputSubsystem& input, Core::System& system);

/// Once per displayed frame, from the thread that presents.
void AcreTasFrame();

/// At exit: a recording is written to script0-1.txt and record.txt.
void AcreTasFinish();
