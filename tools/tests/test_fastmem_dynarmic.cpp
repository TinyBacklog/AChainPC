// SPDX-License-Identifier: GPL-2.0-or-later
// C entry point to dynarmic's InstallFaultHandler for test_fastmem.c.
#include <dynarmic/interface/signal_handler.h>

extern "C" int test_install_dynarmic_handler() {
    return Dynarmic::InstallFaultHandler() ? 1 : 0;
}
