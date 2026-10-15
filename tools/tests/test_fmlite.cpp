// SPDX-License-Identifier: GPL-2.0-or-later
// Emits a differential test of the trimmed guest-memory fast path ("FM-lite").
//
// Two identical guest address spaces are driven with the same random stream:
// one through the shipped recomp_loadN/recomp_storeN, one through the original
// full-check path (memload/memstore). Every returned value, every byte of guest
// memory and the exact sequence of emulator callbacks must match. The memory
// code is lifted out of RuntimeC() verbatim between its markers.
#include <iostream>
#include <string>
#include "core/recompiler/arm64_to_c.h"

int main() {
    const std::string runtime = suyu::recomp::RuntimeC();
    const size_t a = runtime.find("/* acre: guest memory begin */");
    const size_t b = runtime.find("/* acre: guest memory end */");
    if (a == std::string::npos || b == std::string::npos) {
        std::cerr << "guest memory markers missing from RuntimeC()\n";
        return 1;
    }
    std::cout << suyu::recomp::RuntimeH() << "\n#include <stdio.h>\n#include <stdlib.h>\n";
    std::cout << "volatile int acre_fps60 = 0;\n";
    std::cout << "static uint8_t* memptr(GuestContext* c, uint64_t va, uint64_t sz){(void)c;(void)va;(void)sz;return 0;}\n";
    std::cout << runtime.substr(a, b - a) << "\n";
    std::cout << R"C(
#define PAGES 64
#define AS_SIZE (PAGES * 4096ULL)
typedef struct Space {
    uint8_t host[PAGES][4096];
    uint64_t table[PAGES * 4];
    uint64_t log[1 << 16]; int nlog;   /* emulator callbacks, in order */
    RecompHostMem bridge;
    GuestContext ctx;
} Space;
static Space A, B;
static uint64_t cb_load(void* u, uint64_t va, uint32_t s){
    Space* sp = (Space*)u; if (sp->nlog < (1 << 16) - 2) { sp->log[sp->nlog++] = va; sp->log[sp->nlog++] = s; }
    return va * 2654435761u + s;   /* deterministic "MMIO" value */
}
static void cb_store(void* u, uint64_t va, uint32_t s, uint64_t v){
    Space* sp = (Space*)u; if (sp->nlog < (1 << 16) - 3) { sp->log[sp->nlog++] = va; sp->log[sp->nlog++] = s; sp->log[sp->nlog++] = v; }
}
static void setup(Space* sp, int fm) {
    memset(sp, 0, sizeof *sp);
    for (uint64_t p = 0; p < PAGES; ++p) {
        uint64_t host_page = (p * 7) % PAGES;   /* scattered, like real memory */
        sp->table[p * 4] = (uint64_t)(uintptr_t)sp->host[host_page] - p * 4096;
    }
    sp->table[5 * 4] = 0;                          /* unmapped / GPU-tracked */
    sp->table[6 * 4] |= 1;                         /* attribute bits must be masked */
    sp->bridge.user = sp; sp->bridge.load = cb_load; sp->bridge.store = cb_store;
    sp->bridge.page_table = sp->table; sp->bridge.address_space_size = AS_SIZE;
    sp->ctx.host_mem = &sp->bridge;
    if (fm) { sp->ctx.fm_table = (const uint8_t*)sp->table; sp->ctx.fm_limit = AS_SIZE; }
}
static uint64_t rng = 88172645463325252ULL;
static uint64_t next(void){ rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static uint64_t address(void){
    uint64_t r = next();
    switch (r % 8) {
    case 0: return 5 * 4096 + (r >> 8) % 4096;                       /* unmapped page */
    case 1: return ((r >> 8) % PAGES) * 4096 + 4096 - 1 - (r >> 20) % 7; /* near page end */
    case 2: return AS_SIZE - 8 + (r >> 8) % 16;                      /* around the limit */
    case 3: return 0xFF00000000000000ULL | ((r >> 8) % AS_SIZE);      /* tagged high bits */
    case 4: return (r >> 8) % 64;                                    /* near zero */
    default: return (r >> 8) % AS_SIZE;                              /* anywhere mapped */
    }
}
static int fails;
int main(void) {
    setup(&A, 1); setup(&B, 0);
    static const unsigned sizes[4] = {1, 2, 4, 8};
    for (long i = 0; i < 2000000 && !fails; ++i) {
        const uint64_t va = address();
        const unsigned sz = sizes[next() % 4];
        const uint64_t value = next();
        if (next() & 1) {
            uint64_t ra = 0, rb = memload(&B.ctx, va, sz);
            switch (sz) {
            case 1: ra = recomp_load8(&A.ctx, va); break;
            case 2: ra = recomp_load16(&A.ctx, va); break;
            case 4: ra = recomp_load32(&A.ctx, va); break;
            default: ra = recomp_load64(&A.ctx, va); break;
            }
            if (ra != rb) { printf("FAIL load%u at %#llx: %#llx vs %#llx\n", sz * 8, (unsigned long long)va, (unsigned long long)ra, (unsigned long long)rb); ++fails; }
        } else {
            switch (sz) {
            case 1: recomp_store8(&A.ctx, va, value); break;
            case 2: recomp_store16(&A.ctx, va, value); break;
            case 4: recomp_store32(&A.ctx, va, value); break;
            default: recomp_store64(&A.ctx, va, value); break;
            }
            memstore(&B.ctx, va, sz, value);
        }
        if (A.nlog >= (1 << 16) - 8) { A.nlog = B.nlog = 0; }
    }
    if (memcmp(A.host, B.host, sizeof A.host) != 0) { printf("FAIL guest memory differs\n"); ++fails; }
    if (A.nlog != B.nlog || memcmp(A.log, B.log, sizeof(uint64_t) * (size_t)A.nlog) != 0) { printf("FAIL callback log differs (%d vs %d)\n", A.nlog, B.nlog); ++fails; }
    /* With the fast path disabled (fm_limit = 0) everything takes the old path. */
    setup(&A, 0); setup(&B, 0);
    if (recomp_load32(&A.ctx, 2 * 4096) != memload(&B.ctx, 2 * 4096, 4)) { printf("FAIL disabled path\n"); ++fails; }
    if (fails) return 1;
    printf("fm-lite: ok (2000000 ops)\n");
    return 0;
}
)C";
    return 0;
}
