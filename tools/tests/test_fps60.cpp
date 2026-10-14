// SPDX-License-Identifier: GPL-2.0-or-later
// Emits an executable test of the 60fps timer hooks (game/patches/fps60.c),
// compiled against the real generated runtime header with a fake guest memory.
#include <iostream>
#include "core/recompiler/arm64_to_c.h"

int main() {
    std::cout << suyu::recomp::RuntimeH() << "\n";
    std::cout << R"C(
#include <stdio.h>
#include <math.h>
volatile int acre_fps60 = 1;
uint64_t g_module_base_main = 0x80000000ULL;
/* A few words of guest memory, keyed by address. */
static uint64_t g_addr[64]; static uint32_t g_val[64]; static int g_n;
static uint64_t fake_load(void* u, uint64_t va, uint32_t size) {
    (void)u; (void)size;
    for (int i = 0; i < g_n; ++i) if (g_addr[i] == va) return g_val[i];
    return 0;
}
static void fake_store(void* u, uint64_t va, uint32_t size, uint64_t v) {
    (void)u; (void)size;
    for (int i = 0; i < g_n; ++i) if (g_addr[i] == va) { g_val[i] = (uint32_t)v; return; }
    g_addr[g_n] = va; g_val[g_n++] = (uint32_t)v;
}
#include "fps60.c"
#define TIMER 0x1000ULL
static RecompHostMem g_host = {0, fake_load, fake_store, 0, 0, 0};
static float getf(uint64_t va) { uint32_t b = (uint32_t)fake_load(0, va, 4); float f; memcpy(&f, &b, 4); return f; }
static void setf(uint64_t va, float f) { uint32_t b; memcpy(&b, &f, 4); fake_store(0, va, 4, b); }
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
static GuestContext ctx(uint64_t w21) {
    GuestContext c; memset(&c, 0, sizeof c);
    c.host_mem = &g_host; c.x[19] = TIMER; c.x[21] = w21; return c;
}
static void reset(float ms, float override) {
    g_n = 0; setf(TIMER + 0xDC, 0.03f); setf(TIMER + 0xE0, ms); setf(TIMER + 0xD4, 0.0f);
    setf(g_module_base_main + 0xDADC5CULL, override);
}
int main(void) {
    /* 60fps: step 0.5, whole ticks alternate 0, 1. */
    reset(16.6667f, -1.0f);
    GuestContext c = ctx(1);
    CHECK(acre_fps60_step(&c) == 1);
    CHECK(fabsf(getf(TIMER + 0xD0) - 0.5f) < 1e-3f);
    CHECK(fake_load(0, TIMER + 0xD8, 4) == 0);
    CHECK(c.pc == g_module_base_main + 0x91DCB4ULL);
    acre_fps60_step(&c);
    CHECK(fake_load(0, TIMER + 0xD8, 4) == 1);
    /* 30fps: step 1, one tick. */
    reset(33.3333f, -1.0f); c = ctx(1); acre_fps60_step(&c);
    CHECK(fabsf(getf(TIMER + 0xD0) - 1.0f) < 1e-3f);
    CHECK(fake_load(0, TIMER + 0xD8, 4) == 1);
    /* Long frame: clamped to 2. Very short frame: clamped to 1/16. */
    reset(100.0f, -1.0f); c = ctx(1); acre_fps60_step(&c);
    CHECK(getf(TIMER + 0xD0) == 2.0f);
    reset(1.0f, -1.0f); c = ctx(1); acre_fps60_step(&c);
    CHECK(getf(TIMER + 0xD0) == 1.0f / 16.0f);
    /* Cutscene (w21 == 0): measured step, override ignored. */
    reset(16.6667f, 0.9f); c = ctx(0); acre_fps60_step(&c);
    CHECK(fabsf(getf(TIMER + 0xD0) - 0.5f) < 1e-3f);
    /* Gameplay with audio-sync override: relative correction. */
    reset(16.6667f, 0.9f); c = ctx(1); acre_fps60_step(&c);
    CHECK(fabsf(getf(TIMER + 0xD0) - 0.45f) < 1e-3f);
    /* First sample: only when the previous tick (x8) is zero. */
    reset(0.0f, -1.0f); c = ctx(1); c.x[8] = 5;
    CHECK(acre_fps60_first_sample(&c) == 0);
    CHECK(c.pc == 0);
    c.x[8] = 0;
    CHECK(acre_fps60_first_sample(&c) == 1);
    CHECK(getf(TIMER + 0xD0) == 1.0f);
    CHECK(fake_load(0, TIMER + 0xD8, 4) == 1);
    CHECK(c.pc == g_module_base_main + 0x91DCB4ULL);
    if (fails) return 1;
    printf("fps60 hooks: ok\n");
    return 0;
}
)C";
    return 0;
}
