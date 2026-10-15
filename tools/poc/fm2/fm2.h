/* SPDX-License-Identifier: GPL-2.0-or-later
 * FM2 proof of concept (not part of the build): inline guest accesses into a
 * host arena with `asm goto`, recovered from faults through an exception
 * table. See docs/superpowers/specs/2026-10-15-fastmem-arena-design.md.
 *
 * Each access is `cmp limit / jae slow / mov`, all inside one asm statement.
 * The limit is not global: every function that accesses guest memory owns a
 * 64-bit limit word in the writable section acre_fml (FM2_BLOCK_LIMIT), and
 * its accesses compare against that word in memory. The `mov` is recorded in
 * the section acre_fmx as three 32-bit self-relative offsets: the instruction,
 * the C label `slow` of the same access, and the limit word. The SIGSEGV
 * handler looks the faulting RIP up in that table, counts the fault against
 * the access and resumes at the label; once one access has faulted
 * fm2_demote_after() times, the handler stores 0 to its function's limit word,
 * and from then on every access of that function takes the slow path without
 * faulting. The slow path runs as ordinary code, outside the signal handler. */
#ifndef FM2_POC_H
#define FM2_POC_H
#include <stdint.h>
#include <stddef.h>

typedef struct Ctx {
    uint8_t* arena; /* host address of guest address 0 */
} Ctx;

/* Initial value of every limit word: arena size minus 7. `a < limit` then
   guarantees a + 8 <= size, so no access of any width reaches past the arena,
   and an address near 2^64 cannot wrap. The last 7 bytes are reached through
   the slow path. 0 = off for that function. The runtime can rewrite all words
   at start (fm2_set_all_limits), for a kill switch or another arena size. */
#ifndef FM2_LIMIT_INIT
#define FM2_LIMIT_INIT ((1ull << 39) - 7)
#endif

/* Slow paths, supplied by the user of this header. */
uint64_t fm2_slow_load(Ctx* c, uint64_t a, unsigned size);
void fm2_slow_store(Ctx* c, uint64_t a, unsigned size, uint64_t v);

/* Handler and table (fm2_runtime.c). */
int fm2_install(void);                /* 0 on success; idempotent */
void fm2_set_all_limits(uint64_t v);  /* before the guest runs, or from tests;
                                         also clears the fault counts */
void fm2_set_demote_after(unsigned n); /* faults per access before its function
                                          is demoted; 0 = never (default 16) */
unsigned long fm2_faults(void);       /* faults recovered so far */
unsigned long fm2_demotions(void);    /* limit words set to 0 by the handler */
size_t fm2_table_entries(void);
size_t fm2_limit_words(void);
double fm2_init_seconds(void);

typedef uint8_t __attribute__((may_alias)) fm2_u8;
typedef uint16_t __attribute__((may_alias)) fm2_u16;
typedef uint32_t __attribute__((may_alias)) fm2_u32;
typedef uint64_t __attribute__((may_alias)) fm2_u64;

/* One per function that accesses guest memory, before its first access. */
#define FM2_BLOCK_LIMIT \
    static uint64_t fm2_lim __attribute__((section("acre_fml"), aligned(8), used)) = FM2_LIMIT_INIT

#define FM2_ENTRY(label)                                                  \
    ".pushsection acre_fmx,\"a\"\n\t.balign 4\n\t"                        \
    ".long 1b - ., %l[" #label "] - ., %c[lim] - .\n\t.popsection"

#define FM2_LOAD(name, T, insn, size)                                                 \
    static inline __attribute__((always_inline)) uint64_t name##_at(Ctx* c, uint64_t a, \
                                                                    uint64_t* lim) {    \
        uint64_t v;                                                                    \
        asm goto("cmpq %c[lim](%%rip), %[a]\n\tjae %l[slow]\n"                         \
                 "1: " insn "\n\t" FM2_ENTRY(slow)                                    \
                 : [v] "=r"(v)                                                         \
                 : [m] "m"(*(const T*)(c->arena + a)), [a] "r"(a), [lim] "i"(lim)      \
                 : "cc" : slow);                                                       \
        return v;                                                                      \
    slow:                                                                              \
        return fm2_slow_load(c, a, size);                                              \
    }
FM2_LOAD(fm2_load8, fm2_u8, "movzbl %[m], %k[v]", 1)
FM2_LOAD(fm2_load16, fm2_u16, "movzwl %[m], %k[v]", 2)
FM2_LOAD(fm2_load32, fm2_u32, "movl %[m], %k[v]", 4)
FM2_LOAD(fm2_load64, fm2_u64, "movq %[m], %q[v]", 8)

#define FM2_STORE(name, T, insn, size)                                                       \
    static inline __attribute__((always_inline)) void name##_at(Ctx* c, uint64_t a, uint64_t v, \
                                                                uint64_t* lim) {              \
        asm goto("cmpq %c[lim](%%rip), %[a]\n\tjae %l[slow]\n"                               \
                 "1: " insn "\n\t" FM2_ENTRY(slow)                                          \
                 : [m] "=m"(*(T*)(c->arena + a))                                             \
                 : [v] "r"(v), [a] "r"(a), [lim] "i"(lim)                                    \
                 : "cc" : slow);                                                             \
        return;                                                                              \
    slow:                                                                                    \
        fm2_slow_store(c, a, size, v);                                                       \
    }
FM2_STORE(fm2_store8, fm2_u8, "movb %b[v], %[m]", 1)
FM2_STORE(fm2_store16, fm2_u16, "movw %w[v], %[m]", 2)
FM2_STORE(fm2_store32, fm2_u32, "movl %k[v], %[m]", 4)
FM2_STORE(fm2_store64, fm2_u64, "movq %q[v], %[m]", 8)

/* Accesses against the enclosing function's FM2_BLOCK_LIMIT. */
#define fm2_load8(c, a) fm2_load8_at((c), (a), &fm2_lim)
#define fm2_load16(c, a) fm2_load16_at((c), (a), &fm2_lim)
#define fm2_load32(c, a) fm2_load32_at((c), (a), &fm2_lim)
#define fm2_load64(c, a) fm2_load64_at((c), (a), &fm2_lim)
#define fm2_store8(c, a, v) fm2_store8_at((c), (a), (v), &fm2_lim)
#define fm2_store16(c, a, v) fm2_store16_at((c), (a), (v), &fm2_lim)
#define fm2_store32(c, a, v) fm2_store32_at((c), (a), (v), &fm2_lim)
#define fm2_store64(c, a, v) fm2_store64_at((c), (a), (v), &fm2_lim)

#endif
