// SPDX-License-Identifier: GPL-2.0-or-later
// 60fps hooks for Astral Chain 1.0.1 (main module), called from the lifted code
// when acre_fps60 is set. See fps60.patches for where they sit.
//
// The game advances by a "step" measured in 30fps frames. Its timer (x19 at
// main+0x91DBB0) normally clamps the step to [1, 2] and pins it at 1.0 while a
// cutscene plays, which at 60fps runs everything, cutscenes most visibly, at
// double speed. These hooks compute the step from the measured frame time
// instead, keep the fractional remainder, and expose the whole 30fps ticks of
// the frame for the counters that need an integer.
//
// Timer object fields used here:
//   +0xD0 step (float)        +0xD4 fractional remainder (float)
//   +0xD8 whole ticks (u32)   +0xDC ms -> step factor (float, 0.03)
//   +0xE0 measured ms (float) +0xE4 frame counter (u32)
#include <math.h>
#include <stdint.h>
#include <string.h>
#ifndef SUYU_RECOMP_RUNTIME_H  // already included by the hook test
#include "recomp_runtime.h"
#endif

#define ACRE_TIMER_DONE 0x91DCB4ULL     // stores the tick and returns
#define ACRE_STEP_OVERRIDE 0xDADC5CULL  // float, > 0 while audio sync drives the step

extern uint64_t g_module_base_main;

// Called by the lifted code; it declares them itself at each call site.
int acre_fps60_first_sample(GuestContext* c);
int acre_fps60_step(GuestContext* c);

static uint32_t load32(GuestContext* c, uint64_t va) {
    return (uint32_t)c->host_mem->load(c->host_mem->user, va, 4);
}
static void store32(GuestContext* c, uint64_t va, uint32_t value) {
    c->host_mem->store(c->host_mem->user, va, 4, value);
}
static float loadf(GuestContext* c, uint64_t va) {
    const uint32_t bits = load32(c, va);
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}
static void storef(GuestContext* c, uint64_t va, float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    store32(c, va, bits);
}

// cbz x8 at 0x91DBDC: x8 is the previous tick, zero on the first frame.
int acre_fps60_first_sample(GuestContext* c) {
    if (c->x[8] != 0) return 0;
    const uint64_t timer = c->x[19];
    storef(c, timer + 0xD0, 1.0f);
    storef(c, timer + 0xD4, 0.0f);
    store32(c, timer + 0xD8, 1);
    store32(c, timer + 0xE4, 0);
    c->pc = g_module_base_main + ACRE_TIMER_DONE;
    return 1;
}

// cbz w21 at 0x91DC10: w21 == 0 selects the fixed cutscene step, which is
// exactly what must not happen at 60fps.
int acre_fps60_step(GuestContext* c) {
    const uint64_t timer = c->x[19];
    float step = loadf(c, timer + 0xE0) * loadf(c, timer + 0xDC);
    const float override = loadf(c, g_module_base_main + ACRE_STEP_OVERRIDE);
    if ((uint32_t)c->x[21] != 0 && override > 0.0f) {
        step *= override;  // audio sync, relative to real time
    }
    // Some callers divide by the step; never let it reach zero.
    if (!(step >= 1.0f / 16.0f)) step = 1.0f / 16.0f;
    if (step > 2.0f) step = 2.0f;

    // A frame of 33.3333 ms is 0.999999 steps: without a little slack the whole
    // ticks would come out 0, 2, 0, 2 at a steady 30fps instead of 1, 1, 1.
    float remainder = loadf(c, timer + 0xD4) + step;
    const float ticks = floorf(remainder + 1.0f / 1024.0f);
    remainder -= ticks;

    storef(c, timer + 0xD0, step);
    storef(c, timer + 0xD4, remainder);
    store32(c, timer + 0xD8, (uint32_t)ticks);
    store32(c, timer + 0xE4, 0);
    c->pc = g_module_base_main + ACRE_TIMER_DONE;
    return 1;
}
