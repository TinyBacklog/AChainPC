// SPDX-License-Identifier: GPL-2.0-or-later
// The FM-lite differential test, run through the RECOMP_FM_INLINE fast path
// that blocks expand in place instead of the out-of-line helpers.
#define FMLITE_INLINE 1
#include "test_fmlite.cpp"
