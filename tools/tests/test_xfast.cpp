// SPDX-License-Identifier: GPL-2.0-or-later
// Emits a test of the exclusive fast path of the generated runtime: LDXR/STXR
// and friends on plain memory without the emulator's monitor. Checks every
// width, the 32-bit pair, CLREX, a store without a reservation, a reservation
// lost to another writer, the cases that must still go to the emulator, and
// contended increments from several threads. The guest memory and exclusive
// code are lifted out of RuntimeC() verbatim between their markers.
#include <iostream>
#include <string>
#include "core/recompiler/arm64_to_c.h"

namespace {
std::string Between(const std::string& text, const char* begin, const char* end) {
    const size_t a = text.find(begin);
    const size_t b = text.find(end);
    if (a == std::string::npos || b == std::string::npos) {
        std::cerr << "markers missing from RuntimeC(): " << begin << "\n";
        std::exit(1);
    }
    return text.substr(a, b - a);
}
} // namespace

int main() {
    const std::string runtime = suyu::recomp::RuntimeC();
    std::cout << suyu::recomp::RuntimeH() << "\n#include <stdio.h>\n#include <stdlib.h>\n#include <pthread.h>\n";
    std::cout << "volatile int acre_fps60 = 0;\n";
    std::cout << "static uint8_t* memptr(GuestContext* c, uint64_t va, uint64_t sz){(void)c;(void)va;(void)sz;return 0;}\n";
    std::cout << Between(runtime, "/* acre: guest memory begin */", "/* acre: guest memory end */") << "\n";
    std::cout << Between(runtime, "/* acre: exclusive begin */", "/* acre: exclusive end */") << "\n";
    std::cout << R"C(
#define PAGES 16
#define AS_SIZE (PAGES * 4096ULL)
typedef struct Space {
    uint8_t host[PAGES][4096];
    uint64_t table[PAGES * 4];
    RecompHostMem bridge;
    int host_calls;
    uint32_t last_host_insn;
} Space;
static Space S;
static int failures;
#define CHECK(cond) do { if (!(cond)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

/* Stands in for the emulator's monitor: records the call and claims it. */
static int cb_exclusive(void* u, uint32_t insn) {
    Space* sp = (Space*)u; ++sp->host_calls; sp->last_host_insn = insn; return 1;
}
static void setup(void) {
    memset(&S, 0, sizeof S);
    for (uint64_t p = 0; p < PAGES; ++p)
        S.table[p * 4] = (uint64_t)(uintptr_t)S.host[(p * 7) % PAGES] - p * 4096;
    S.table[5 * 4] = 0;   /* unmapped or GPU-tracked: the emulator's */
    S.bridge.user = &S; S.bridge.exclusive = cb_exclusive;
    S.bridge.page_table = S.table; S.bridge.address_space_size = AS_SIZE;
}
static GuestContext* context(void) {
    GuestContext* c = calloc(1, sizeof *c);
    c->host_mem = &S.bridge;
    c->fm_table = (const uint8_t*)S.table; c->fm_limit = AS_SIZE;
    return c;
}
static uint8_t* host(uint64_t va) { return S.host[((va >> 12) * 7) % PAGES] + (va & 0xfff); }

/* Encodings, with Xn = x2 (address), Xt = x0, Xt2 = x3, Ws = w1. */
#define LDXR(size)  (((uint32_t)(size) << 30) | 0x085F7C00U | (2 << 5))
#define LDAXR(size) (((uint32_t)(size) << 30) | 0x085FFC00U | (2 << 5))
#define STXR(size)  (((uint32_t)(size) << 30) | 0x08007C00U | (1 << 16) | (2 << 5))
#define STLXR(size) (((uint32_t)(size) << 30) | 0x0800FC00U | (1 << 16) | (2 << 5))
#define LDXP(size)  (((uint32_t)(size) << 30) | 0x087F0000U | (3 << 10) | (2 << 5))
#define STXP(size)  (((uint32_t)(size) << 30) | 0x08200000U | (1 << 16) | (3 << 10) | (2 << 5))
#define CLREX 0xD5033F5FU
#define MRS_CNTPCT 0xD53BE020U

static void exec(GuestContext* c, uint32_t insn) { recomp_exclusive(c, insn, 0x1000); CHECK(!c->halted); }

static void widths(void) {
    for (uint32_t size = 0; size < 4; ++size) {
        setup();
        GuestContext* c = context();
        const uint32_t n = 1u << size;
        const uint64_t va = 0x2040;
        memset(host(va), 0x5A, 8);
        c->x[2] = va;
        exec(c, size & 1 ? LDAXR(size) : LDXR(size));
        const uint64_t mask = n == 8 ? ~0ULL : (1ULL << (8 * n)) - 1;
        CHECK(c->x[0] == (0x5A5A5A5A5A5A5A5AULL & mask));
        c->x[0] = 0x1122334455667788ULL;
        exec(c, size & 1 ? STLXR(size) : STXR(size));
        CHECK(c->x[1] == 0);
        uint64_t stored = 0; memcpy(&stored, host(va), n);
        CHECK(stored == (0x1122334455667788ULL & mask));
        CHECK(host(va)[n] == 0x5A || n == 8);   /* nothing past the width */
        c->x[0] = 0;
        exec(c, STXR(size));
        CHECK(c->x[1] == 1);                     /* a store consumes the reservation */
        CHECK(S.host_calls == 0);
        free(c);
    }
}

static void pair32(void) {
    setup();
    GuestContext* c = context();
    c->x[2] = 0x3008;
    memcpy(host(0x3008), "\x01\x02\x03\x04\x05\x06\x07\x08", 8);
    exec(c, LDXP(2));
    CHECK(c->x[0] == 0x04030201ULL && c->x[3] == 0x08070605ULL);
    c->x[0] = 0xAABBCCDDULL; c->x[3] = 0x11223344ULL;
    exec(c, STXP(2));
    CHECK(c->x[1] == 0);
    uint64_t stored; memcpy(&stored, host(0x3008), 8);
    CHECK(stored == 0x11223344AABBCCDDULL);
    CHECK(S.host_calls == 0);
    free(c);
}

static void lost_reservations(void) {
    setup();
    GuestContext* c = context();
    c->x[2] = 0x4000;
    /* Another writer changes the word between load and store. */
    exec(c, LDXR(3));
    host(0x4000)[0] ^= 1;
    c->x[0] = 77;
    exec(c, STXR(3));
    CHECK(c->x[1] == 1);
    CHECK(host(0x4000)[0] == 1);
    /* CLREX drops the reservation. */
    exec(c, LDXR(3));
    exec(c, CLREX);
    exec(c, STXR(3));
    CHECK(c->x[1] == 1);
    /* A store with no load before it fails. */
    exec(c, STXR(2));
    CHECK(c->x[1] == 1);
    /* A store to another address or of another width fails. */
    exec(c, LDXR(3));
    c->x[2] = 0x4008;
    exec(c, STXR(3));
    CHECK(c->x[1] == 1);
    c->x[2] = 0x4000;
    exec(c, LDXR(3));
    exec(c, STXR(2));
    CHECK(c->x[1] == 1);
    /* The emulator clears the reservation on every entry. */
    exec(c, LDXR(3));
    c->excl_kind = 0;
    exec(c, STXR(3));
    CHECK(c->x[1] == 1);
    CHECK(S.host_calls == 0);
    CHECK(host(0x4000)[0] == 1);
    free(c);
}

static void emulator_cases(void) {
    setup();
    GuestContext* c = context();
    /* A page the fast path cannot see: load and store go to the monitor. */
    c->x[2] = 0x5010;
    exec(c, LDXR(2));
    CHECK(S.host_calls == 1 && S.last_host_insn == LDXR(2));
    exec(c, STXR(2));
    CHECK(S.host_calls == 2 && S.last_host_insn == STXR(2));
    /* So does CLREX while the monitor holds the reservation, once. */
    exec(c, LDXR(2));
    exec(c, CLREX);
    CHECK(S.host_calls == 4 && S.last_host_insn == CLREX);
    exec(c, CLREX);
    CHECK(S.host_calls == 4);
    /* Unaligned addresses (the monitor raises what the hardware would). */
    c->x[2] = 0x2001;
    exec(c, LDXR(2));
    CHECK(S.host_calls == 5);
    /* 128-bit pairs. */
    c->x[2] = 0x2000;
    exec(c, LDXP(3));
    CHECK(S.host_calls == 6);
    /* System registers share the helper. */
    exec(c, MRS_CNTPCT);
    CHECK(S.host_calls == 7 && S.last_host_insn == MRS_CNTPCT);
    /* A store after a monitor load asks the monitor even on a plain page. */
    c->x[2] = 0x5010;
    exec(c, LDXR(3));
    c->x[2] = 0x2000;
    exec(c, STXR(3));
    CHECK(S.host_calls == 9);
    /* Fast path off. */
    c->fm_limit = 0;
    exec(c, LDXR(3));
    CHECK(S.host_calls == 10);
    free(c);
}

#define THREADS 4
#define ROUNDS 200000
static void* increment(void* arg) {
    GuestContext* c = (GuestContext*)arg;
    for (int i = 0; i < ROUNDS; ++i) {
        do {
            exec(c, LDAXR(3));
            c->x[0] += 1;
            exec(c, STLXR(3));
        } while (c->x[1]);
    }
    return 0;
}
static void contended(void) {
    setup();
    pthread_t threads[THREADS];
    GuestContext* contexts[THREADS];
    memset(host(0x6000), 0, 8);
    for (int t = 0; t < THREADS; ++t) {
        contexts[t] = context();
        contexts[t]->x[2] = 0x6000;
        pthread_create(&threads[t], 0, increment, contexts[t]);
    }
    for (int t = 0; t < THREADS; ++t) { pthread_join(threads[t], 0); free(contexts[t]); }
    uint64_t total; memcpy(&total, host(0x6000), 8);
    CHECK(total == (uint64_t)THREADS * ROUNDS);
    CHECK(S.host_calls == 0);
}

int main(void) {
    widths();
    pair32();
    lost_reservations();
    emulator_cases();
    contended();
    if (failures) { printf("%d failures\n", failures); return 1; }
    printf("exclusive fast path: widths, pair, reservations, emulator cases and %d contended increments passed\n",
           THREADS * ROUNDS);
    return 0;
}
)C";
}
