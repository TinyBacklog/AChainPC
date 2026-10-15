/* SPDX-License-Identifier: GPL-2.0-or-later
 * Fastmem arena (FM2): inline guest accesses for the recompiled C. Plain C
 * (gnu11) for clang; the runtime side is recomp_fastmem.cpp.
 *
 * Each access is `cmp limit / jae slow / mov`, all inside one asm statement.
 * Every function that accesses guest memory owns a 64-bit limit word in the
 * writable section acre_fml (RECOMP_BLOCK_LIMIT), and its accesses compare
 * against that word in memory, so the compiler cannot keep the limit in a
 * register across a loop and a demotion takes effect on the next access. The
 * `mov` is recorded in the section acre_fmx as three 32-bit self-relative
 * offsets: the instruction, the C label `slow` of the same access, and the
 * limit word. On a fault the handler resumes at the label, which calls the
 * slow path as ordinary code; after enough faults of one access it stores 0
 * to the function's limit word.
 *
 * The includer defines, before including this header:
 *   RECOMP_FX_CTX                   the context type
 *   RECOMP_FX_ARENA(c)              host address of guest address 0 (uint8_t*)
 *   RECOMP_FX_SLOW_LOAD(c, a, n)    slow load of n bytes, zero-extended
 *   RECOMP_FX_SLOW_STORE(c, a, n, v) slow store of the low n bytes of v */
#ifndef ACRE_RECOMP_FASTMEM_ACCESS_H
#define ACRE_RECOMP_FASTMEM_ACCESS_H
#include <stdint.h>

/* Initial value of every limit word: the arena size minus 7. `a < limit`
   then guarantees a + 8 <= size, so no access of any width reaches past the
   arena, and an address near 2^64 cannot wrap. The last 7 bytes go through
   the slow path. 0 = every access of that function takes the slow path. */
#ifndef RECOMP_FM_LIMIT_INIT
#define RECOMP_FM_LIMIT_INIT ((1ull << 39) - 7)
#endif

typedef uint8_t __attribute__((may_alias)) recomp_fx_u8;
typedef uint16_t __attribute__((may_alias)) recomp_fx_u16;
typedef uint32_t __attribute__((may_alias)) recomp_fx_u32;
typedef uint64_t __attribute__((may_alias)) recomp_fx_u64;

/* One per function that accesses guest memory, before its first access. */
#define RECOMP_BLOCK_LIMIT \
    static uint64_t recomp_fm_lim __attribute__((section("acre_fml"), aligned(8), used)) = RECOMP_FM_LIMIT_INIT

#define RECOMP_FX_ENTRY                                                   \
    ".pushsection acre_fmx,\"a\"\n\t.balign 4\n\t"                        \
    ".long 1b - ., %l[slow] - ., %c[lim] - .\n\t.popsection"

#define RECOMP_FX_LOAD(name, T, insn, size)                                                       \
    static inline __attribute__((always_inline)) uint64_t name(RECOMP_FX_CTX* c, uint64_t a,        \
                                                               uint64_t* lim) {                     \
        uint64_t v;                                                                                \
        __asm__ goto("cmpq %c[lim](%%rip), %[a]\n\tjae %l[slow]\n"                                 \
                     "1: " insn "\n\t" RECOMP_FX_ENTRY                                            \
                     : [v] "=r"(v)                                                                 \
                     : [m] "m"(*(const T*)(RECOMP_FX_ARENA(c) + a)), [a] "r"(a), [lim] "i"(lim)    \
                     : "cc" : slow);                                                               \
        return v;                                                                                  \
    slow:                                                                                          \
        return RECOMP_FX_SLOW_LOAD(c, a, size);                                                    \
    }
RECOMP_FX_LOAD(recomp_fx_load8, recomp_fx_u8, "movzbl %[m], %k[v]", 1)
RECOMP_FX_LOAD(recomp_fx_load16, recomp_fx_u16, "movzwl %[m], %k[v]", 2)
RECOMP_FX_LOAD(recomp_fx_load32, recomp_fx_u32, "movl %[m], %k[v]", 4)
RECOMP_FX_LOAD(recomp_fx_load64, recomp_fx_u64, "movq %[m], %q[v]", 8)

#define RECOMP_FX_STORE(name, T, insn, size)                                                    \
    static inline __attribute__((always_inline)) void name(RECOMP_FX_CTX* c, uint64_t a,         \
                                                           uint64_t v, uint64_t* lim) {          \
        __asm__ goto("cmpq %c[lim](%%rip), %[a]\n\tjae %l[slow]\n"                               \
                     "1: " insn "\n\t" RECOMP_FX_ENTRY                                          \
                     : [m] "=m"(*(T*)(RECOMP_FX_ARENA(c) + a))                                   \
                     : [v] "r"(v), [a] "r"(a), [lim] "i"(lim)                                    \
                     : "cc" : slow);                                                             \
        return;                                                                                  \
    slow:                                                                                        \
        RECOMP_FX_SLOW_STORE(c, a, size, v);                                                     \
    }
RECOMP_FX_STORE(recomp_fx_store8, recomp_fx_u8, "movb %b[v], %[m]", 1)
RECOMP_FX_STORE(recomp_fx_store16, recomp_fx_u16, "movw %w[v], %[m]", 2)
RECOMP_FX_STORE(recomp_fx_store32, recomp_fx_u32, "movl %k[v], %[m]", 4)
RECOMP_FX_STORE(recomp_fx_store64, recomp_fx_u64, "movq %q[v], %[m]", 8)

/* Accesses against the enclosing function's RECOMP_BLOCK_LIMIT. */
#define RECOMP_FX_LD8(c, a) recomp_fx_load8((c), (a), &recomp_fm_lim)
#define RECOMP_FX_LD16(c, a) recomp_fx_load16((c), (a), &recomp_fm_lim)
#define RECOMP_FX_LD32(c, a) recomp_fx_load32((c), (a), &recomp_fm_lim)
#define RECOMP_FX_LD64(c, a) recomp_fx_load64((c), (a), &recomp_fm_lim)
#define RECOMP_FX_ST8(c, a, v) recomp_fx_store8((c), (a), (v), &recomp_fm_lim)
#define RECOMP_FX_ST16(c, a, v) recomp_fx_store16((c), (a), (v), &recomp_fm_lim)
#define RECOMP_FX_ST32(c, a, v) recomp_fx_store32((c), (a), (v), &recomp_fm_lim)
#define RECOMP_FX_ST64(c, a, v) recomp_fx_store64((c), (a), (v), &recomp_fm_lim)

#endif
