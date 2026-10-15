/* SPDX-License-Identifier: GPL-2.0-or-later
 * FM2 proof of concept (not part of the build): inline guest accesses into a
 * host arena with `asm goto`, recovered from faults through an exception
 * table. See docs/superpowers/specs/2026-10-15-fastmem-arena-design.md.
 *
 * Each access is `cmp limit / jae slow / mov`. The `mov` is recorded in the
 * section acre_fmx as a pair of 32-bit self-relative offsets: the instruction
 * and the C label `slow` of the same access. The SIGSEGV handler looks the
 * faulting RIP up in that table and resumes at the label; the slow path then
 * runs as ordinary code, outside the signal handler. */
#ifndef FM2_POC_H
#define FM2_POC_H
#include <stdint.h>
#include <stddef.h>

typedef struct Ctx {
    uint8_t* arena;   /* host address of guest address 0 */
    /* Arena size minus 7: `a < limit` then guarantees a + 8 <= size, so no
       access of any width reaches past the arena, and an address near 2^64
       cannot wrap. The last 7 bytes are reached through the slow path.
       0 = off: every access takes the slow path. */
    uint64_t limit;
} Ctx;

/* Slow paths, supplied by the user of this header. */
uint64_t fm2_slow_load(Ctx* c, uint64_t a, unsigned size);
void fm2_slow_store(Ctx* c, uint64_t a, unsigned size, uint64_t v);

/* Handler and table (fm2_runtime.c). */
int fm2_install(void);                /* 0 on success; idempotent */
unsigned long fm2_faults(void);       /* faults recovered so far */
size_t fm2_table_entries(void);
double fm2_init_seconds(void);

typedef uint8_t __attribute__((may_alias)) fm2_u8;
typedef uint16_t __attribute__((may_alias)) fm2_u16;
typedef uint32_t __attribute__((may_alias)) fm2_u32;
typedef uint64_t __attribute__((may_alias)) fm2_u64;

#define FM2_ENTRY(label) \
    ".pushsection acre_fmx,\"a\"\n\t.balign 4\n\t.long 1b - ., %l[" #label "] - .\n\t.popsection"

#define FM2_LOAD(name, T, insn, size)                                              \
    static inline __attribute__((always_inline)) uint64_t name(Ctx* c, uint64_t a) { \
        uint64_t v;                                                                 \
        if (__builtin_expect(a < c->limit, 1)) {                                    \
            asm goto("1: " insn "\n\t" FM2_ENTRY(slow)                              \
                     : "=r"(v) : "m"(*(const T*)(c->arena + a)) : : slow);          \
            return v;                                                               \
        }                                                                           \
    slow:                                                                           \
        return fm2_slow_load(c, a, size);                                           \
    }
FM2_LOAD(fm2_load8, fm2_u8, "movzbl %1, %k0", 1)
FM2_LOAD(fm2_load16, fm2_u16, "movzwl %1, %k0", 2)
FM2_LOAD(fm2_load32, fm2_u32, "movl %1, %k0", 4)
FM2_LOAD(fm2_load64, fm2_u64, "movq %1, %q0", 8)

#define FM2_STORE(name, T, insn, size)                                                       \
    static inline __attribute__((always_inline)) void name(Ctx* c, uint64_t a, uint64_t v) { \
        if (__builtin_expect(a < c->limit, 1)) {                                              \
            asm goto("1: " insn "\n\t" FM2_ENTRY(slow)                                        \
                     : "=m"(*(T*)(c->arena + a)) : "r"(v) : : slow);                          \
            return;                                                                           \
        }                                                                                     \
    slow:                                                                                     \
        fm2_slow_store(c, a, size, v);                                                        \
    }
FM2_STORE(fm2_store8, fm2_u8, "movb %b1, %0", 1)
FM2_STORE(fm2_store16, fm2_u16, "movw %w1, %0", 2)
FM2_STORE(fm2_store32, fm2_u32, "movl %k1, %0", 4)
FM2_STORE(fm2_store64, fm2_u64, "movq %q1, %0", 8)

#endif
