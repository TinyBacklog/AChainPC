// SPDX-License-Identifier: GPL-2.0-or-later
// Fastmem arena (FM2) runtime: the exception table of the recompiled code's
// inline guest accesses, the SIGSEGV/SIGBUS handler that recovers them, and
// per-function demotion. See docs/superpowers/specs/2026-10-15-fastmem-arena-design.md
// and recomp_fastmem_access.h for the access side.
//
// C linkage and no emulator dependencies, so the lifter tests can link it.
#ifndef ACRE_RECOMP_FASTMEM_H
#define ACRE_RECOMP_FASTMEM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Builds the sorted table from the acre_fmx section and installs the handler
   for SIGSEGV and SIGBUS. `install_first`, when not null, runs before our
   sigaction so that a handler it installs (dynarmic's) ends up below ours in
   the chain; its result is ignored. Runs once; later calls return the first
   result. 0 = installed; 1 = the binary has no inline accesses, nothing
   installed; -1 = failed, every limit word set to 0. */
int acre_fm_install(int (*install_first)(void));

/* Stores `v` to every limit word and clears the fault counts. 0 turns every
   inline access into a helper call; the arena limit re-promotes everything. */
void acre_fm_set_all_limits(uint64_t v);

/* Faults of one access before its function is demoted; 0 = never. */
void acre_fm_set_demote_after(unsigned n);

/* Player settings (astralchain.ini), applied before the guest starts:
   whether FM2 may be used at all, and the demotion threshold. */
void acre_fm_configure(int enabled, unsigned demote_after);
int acre_fm_configured_enabled(void);

unsigned long acre_fm_faults(void);    /* faults recovered so far */
unsigned long acre_fm_demotions(void); /* limit words set to 0 by the handler */
size_t acre_fm_table_entries(void);
size_t acre_fm_limit_words(void);
double acre_fm_init_seconds(void);

/* 1 when the current SIGSEGV action is still our handler. */
int acre_fm_handler_is_ours(void);

/* 1 when the calling thread can take the handler's faults: neither SIGSEGV nor
   SIGBUS is blocked. A synchronous fault on a blocked signal kills the
   process instead of reaching any handler. */
int acre_fm_thread_can_fault(void);

#ifdef __cplusplus
}
#endif

#endif
