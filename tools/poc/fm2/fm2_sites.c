/* SPDX-License-Identifier: GPL-2.0-or-later
 * FM2 proof of concept: exception table at scale. Calls every generated site
 * with an address on a PROT_NONE page; each must fault, be found in the table
 * and fall through to its own slow path. Each site is its own function, so it
 * also has its own limit word. Reports table size and init time. */
#define _GNU_SOURCE
#include <stdatomic.h>
#include <stdio.h>
#include <sys/mman.h>
#include "fm2.h"

extern uint64_t (*const g_sites[])(Ctx*, uint64_t);
extern const unsigned g_nsites;
static atomic_ulong g_slow;

uint64_t fm2_slow_load(Ctx* c, uint64_t a, unsigned size) { (void)c; (void)a; (void)size; atomic_fetch_add(&g_slow, 1); return 1000000; }
void fm2_slow_store(Ctx* c, uint64_t a, unsigned size, uint64_t v) { (void)c; (void)a; (void)size; (void)v; atomic_fetch_add(&g_slow, 1); }

int main(void) {
    uint8_t* arena = mmap(NULL, 8192, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    Ctx c = {arena};
    if (fm2_install() != 0) { printf("FAIL install\n"); return 1; }
    fm2_set_all_limits(8192 - 7);
    printf("sites: %u, table entries: %zu, limit words: %zu, init %.2f ms\n", g_nsites, fm2_table_entries(),
           fm2_limit_words(), fm2_init_seconds() * 1e3);
    unsigned bad = 0;
    for (unsigned i = 0; i < g_nsites; ++i) {
        const unsigned long s0 = atomic_load(&g_slow);
        const uint64_t r = g_sites[i](&c, 0x100);
        const uint64_t want = (i % 2 == 0 ? 1000000u : 0u) + i;
        if (r != want || atomic_load(&g_slow) - s0 != 1) { if (bad++ < 5) printf("FAIL site %u: %llu\n", i, (unsigned long long)r); }
    }
    if (bad || fm2_faults() != g_nsites || fm2_table_entries() < g_nsites || fm2_limit_words() < g_nsites) {
        printf("FAIL: %u bad sites, %lu faults\n", bad, fm2_faults());
        return 1;
    }
    printf("ok: %lu faults recovered\n", fm2_faults());
    return 0;
}
