/* This file is part of the dynarmic project.
 * Copyright (c) 2020 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

namespace Dynarmic {

/// Installs dynarmic's fault handler now rather than when the first JIT with
/// fastmem is created. A host that installs its own SIGSEGV handler and wants
/// it to run first calls this beforehand, so dynarmic's handler ends up below
/// its own in the chain. Returns false where dynarmic has no handler to
/// install or could not install it; calling it again is harmless.
bool InstallFaultHandler();

}  // namespace Dynarmic
