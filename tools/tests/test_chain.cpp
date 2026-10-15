// SPDX-License-Identifier: GPL-2.0-or-later
// Emits an executable test of direct block chaining.
//
// ChainSuccessors rewrites every terminator whose successor is known, so a
// defect here does not fail to compile - it runs the wrong block, or runs the
// right one with the dispatcher's view of the PC stale. Neither shows up in any
// other test, because they all call Translate() directly and never see the
// rewrite.
//
// The test builds the same run of blocks twice, once as emitted and once
// chained, and drives both through a stand-in dispatcher. It then requires that
// every register, the flags, the PC, the halt flag and all of guest memory come
// out identical - and, separately, that the result does not depend on the
// budget, which is the whole point of the budget being safe to tune.
#include <cassert>
#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>
#include "core/recompiler/arm64_to_c.h"

namespace {

// A run of straight-line code with a conditional branch in the middle and a
// return at the end - the shape the rewrite has to get right, including the
// two-successor terminator where both edges are chained.
const std::vector<uint32_t> kProgram = {
    0x91000421,  // 0x00: add  x1, x1, #1
    0x8b020020,  // 0x04: add  x0, x1, x2
    0xf9000060,  // 0x08: str  x0, [x3]
    0xf1000c3f,  // 0x0c: cmp  x1, #3
    0x54000060,  // 0x10: b.eq +12   -> 0x1c
    0x8b010000,  // 0x14: add  x0, x0, x1
    0x14000002,  // 0x18: b    +8    -> 0x20
    0xd1000400,  // 0x1c: sub  x0, x0, #1
    0xd4000001,  // 0x20: svc  #0     <- terminador que escreve o PC e continua
    0xf9400464,  // 0x24: ldr  x4, [x3, #8]
    0x8b040000,  // 0x28: add  x0, x0, x4
    0xf9000460,  // 0x2c: str  x0, [x3, #8]
    0xd65f03c0,  // 0x30: ret
};

// A guest loop. Without it the budget is untestable: a straight run ends on its
// own whatever the budget does, so a chain that never decrements it still looks
// correct. A backward edge is the case the budget exists for - chained without
// a limit it never returns to the dispatcher, and interrupts, pending SVCs and
// the halt request stop being seen.
const std::vector<uint32_t> kLoop = {
    0xd1000400,  // 0x00: sub  x0, x0, #1
    0x8b010021,  // 0x04: add  x1, x1, x1
    0xf100001f,  // 0x08: cmp  x0, #0
    0x54ffffa1,  // 0x0c: b.ne -12  -> 0x00
    0xd65f03c0,  // 0x10: ret
};

// Indirect branches. BLR and RET land on blocks of the same module, so the
// rewrite may chain them through the module's own lookup; the BR at the end
// leaves the module (x30 points outside it), so it must hand back to the
// dispatcher with the PC set. Chained with any budget above the number of
// blocks, the whole run is then a single dispatch.
const std::vector<uint32_t> kCalls = {
    0xaa1e03e5,  // 0x00: mov  x5, x30
    0xd63f00c0,  // 0x04: blr  x6        (x6 = 0x14)
    0x91000400,  // 0x08: add  x0, x0, #1
    0xaa0503fe,  // 0x0c: mov  x30, x5
    0xd61f03c0,  // 0x10: br   x30       -> outside the module
    0x91000800,  // 0x14: add  x0, x0, #2
    0xd65f03c0,  // 0x18: ret            -> 0x08
};

}  // namespace

int main() {
    // Every four-byte offset is a block start, which is the worst case for the
    // rewrite: maximum chaining, and every successor resolvable.
    std::unordered_set<uint64_t> starts;
    for (size_t i = 0; i < kProgram.size(); ++i) starts.insert(i * 4);

    std::cout << R"C(
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct GuestContext {
    uint64_t x[32];
    uint64_t pc;
    uint8_t n,z,c,v;
    int halted;
    uint64_t vreg[32][2];
    uint64_t pending_svc;
    int32_t chain_budget;
} GuestContext;
uint64_t g_module_base = 0;
/* The module's own block lookup, which the rewrite calls for indirect
   branches. Each driver points it at the table it is running. */
typedef void (*BlockFn)(GuestContext*);
static BlockFn* g_lookup = 0;
static uint64_t g_lookup_n = 0;
static int g_lookups = 0;
BlockFn recomp_lookup(uint64_t pc){
    ++g_lookups;
    if (pc % 4 || pc / 4 >= g_lookup_n) return 0;
    return g_lookup[pc / 4];
}
/* Counts calls rather than servicing them: what matters is that chaining does
   not skip the system call, which is exactly what it did before the terminator
   was required to be the last thing in the block. */
static int g_svc_calls = 0;
void recomp_svc(GuestContext* c, unsigned imm){ (void)c; (void)imm; ++g_svc_calls; }
#define MEMSZ 4096
static uint8_t g_mem[MEMSZ];
static uint64_t ml(uint64_t a, unsigned s){ uint64_t v=0; if(a+s<=MEMSZ) memcpy(&v,g_mem+a,s); return v; }
static void ms(uint64_t a, unsigned s, uint64_t v){ if(a+s<=MEMSZ) memcpy(g_mem+a,&v,s); }
uint64_t recomp_load8 (GuestContext*c,uint64_t a){(void)c;return ml(a,1);}
uint64_t recomp_load16(GuestContext*c,uint64_t a){(void)c;return ml(a,2);}
uint64_t recomp_load32(GuestContext*c,uint64_t a){(void)c;return ml(a,4);}
uint64_t recomp_load64(GuestContext*c,uint64_t a){(void)c;return ml(a,8);}
void recomp_store8 (GuestContext*c,uint64_t a,uint64_t v){(void)c;ms(a,1,v);}
void recomp_store16(GuestContext*c,uint64_t a,uint64_t v){(void)c;ms(a,2,v);}
void recomp_store32(GuestContext*c,uint64_t a,uint64_t v){(void)c;ms(a,4,v);}
void recomp_store64(GuestContext*c,uint64_t a,uint64_t v){(void)c;ms(a,8,v);}
)C";

    // The flag helpers ship inside RuntimeH(); lift them rather than restate.
    {
        const std::string rt = suyu::recomp::RuntimeH();
        const size_t a = rt.find("static inline void recomp_set_flags");
        assert(a != std::string::npos && "recomp_set_flags missing from RuntimeH()");
        const size_t b = rt.find("\n}", rt.find("static inline int recomp_cond", a));
        assert(b != std::string::npos && "recomp_cond has no closing brace");
        std::cout << rt.substr(a, b - a + 3) << "\n";
    }

    std::string plain, chained, table;
    for (size_t i = 0; i < kProgram.size(); ++i) {
        const uint64_t address = i * 4;
        std::string body, one;
        bool open = true;
        for (size_t k = i; k < kProgram.size() && open; ++k) {
            one.clear();
            open = suyu::recomp::Translate(kProgram[k], k * 4, one);
            body += one;
            if (!open) break;
            // One instruction per block, so the run exercises every edge.
            body += "    c->pc=g_module_base+0x" + [&] {
                char b[32];
                snprintf(b, sizeof b, "%llx", (unsigned long long)(address + 4));
                return std::string(b);
            }() + "ULL; return;\n";
            break;
        }
        const std::string rewritten =
            suyu::recomp::ChainSuccessors(body, "t", starts);
        plain += "static void plain_" + std::to_string(i) + "(GuestContext* c){\n" + body + "}\n";
        chained += "void blk_t_" + [&] {
            char b[32];
            snprintf(b, sizeof b, "%016llx", (unsigned long long)address);
            return std::string(b);
        }() + "(GuestContext* c){\n" + rewritten + "}\n";
        table += "plain_" + std::to_string(i) + ",";
    }
    // The chained bodies call each other by name, so they must all be declared
    // before any of them is defined.
    for (size_t i = 0; i < kProgram.size(); ++i) {
        char b[32];
        snprintf(b, sizeof b, "%016llx", (unsigned long long)(i * 4));
        std::cout << "void blk_t_" << b << "(GuestContext*);\n";
    }
    std::cout << plain << chained;
    std::cout << "static void (*plains[])(GuestContext*) = {" << table << "};\n";
    std::cout << "static void (*chains[])(GuestContext*) = {";
    for (size_t i = 0; i < kProgram.size(); ++i) {
        char b[32];
        snprintf(b, sizeof b, "%016llx", (unsigned long long)(i * 4));
        std::cout << "blk_t_" << b << ",";
    }
    std::cout << "};\n#define NBLK " << kProgram.size() << "\n";

    // The loop, emitted the same way under its own module name.
    {
        std::unordered_set<uint64_t> loop_starts;
        for (size_t i = 0; i < kLoop.size(); ++i) loop_starts.insert(i * 4);
        std::string bodies;
        for (size_t i = 0; i < kLoop.size(); ++i) {
            const uint64_t address = i * 4;
            std::string body, one;
            const bool open = suyu::recomp::Translate(kLoop[i], address, one);
            body += one;
            if (open) {
                char b[32];
                snprintf(b, sizeof b, "%llx", (unsigned long long)(address + 4));
                body += "    c->pc=g_module_base+0x" + std::string(b) + "ULL; return;\n";
            }
            char name[32];
            snprintf(name, sizeof name, "%016llx", (unsigned long long)address);
            std::cout << "void blk_l_" << name << "(GuestContext*);\n";
            bodies += "void blk_l_" + std::string(name) + "(GuestContext* c){\n" +
                      suyu::recomp::ChainSuccessors(body, "l", loop_starts) + "}\n";
        }
        std::cout << bodies << "static void (*loops[])(GuestContext*) = {";
        for (size_t i = 0; i < kLoop.size(); ++i) {
            char name[32];
            snprintf(name, sizeof name, "%016llx", (unsigned long long)(i * 4));
            std::cout << "blk_l_" << name << ",";
        }
        std::cout << "};\n#define NLOOP " << kLoop.size() << "\n";
    }

    // The indirect-branch program, as plain blocks and as chained ones.
    {
        std::unordered_set<uint64_t> call_starts;
        for (size_t i = 0; i < kCalls.size(); ++i) call_starts.insert(i * 4);
        std::string plain_bodies, chained_bodies;
        for (size_t i = 0; i < kCalls.size(); ++i) {
            const uint64_t address = i * 4;
            std::string body;
            const bool open = suyu::recomp::Translate(kCalls[i], address, body);
            if (open) {
                char b[32];
                snprintf(b, sizeof b, "%llx", (unsigned long long)(address + 4));
                body += "    c->pc=g_module_base+0x" + std::string(b) + "ULL; return;\n";
            }
            char name[32];
            snprintf(name, sizeof name, "%016llx", (unsigned long long)address);
            std::cout << "void blk_k_" << name << "(GuestContext*);\n";
            plain_bodies += "static void callplain_" + std::to_string(i) + "(GuestContext* c){\n" + body + "}\n";
            chained_bodies += "void blk_k_" + std::string(name) + "(GuestContext* c){\n" +
                              suyu::recomp::ChainSuccessors(body, "k", call_starts) + "}\n";
        }
        std::cout << plain_bodies << chained_bodies << "static BlockFn callplains[] = {";
        for (size_t i = 0; i < kCalls.size(); ++i) std::cout << "callplain_" << i << ",";
        std::cout << "};\nstatic BlockFn callchains[] = {";
        for (size_t i = 0; i < kCalls.size(); ++i) {
            char name[32];
            snprintf(name, sizeof name, "%016llx", (unsigned long long)(i * 4));
            std::cout << "blk_k_" << name << ",";
        }
        std::cout << "};\n#define NCALL " << kCalls.size() << "\n";
    }

    // A terminator whose successor has no block must be left exactly as it was.
    // Checked here rather than at run time: with every offset a block start the
    // run-time comparison can never reach this path.
    {
        const std::string sample = "    c->pc=g_module_base+0x9999ULL; return;\n";
        const std::unordered_set<uint64_t> none;
        const bool untouched = suyu::recomp::ChainSuccessors(sample, "t", none) == sample;
        std::cout << "#define UNKNOWN_TARGET_LEFT_ALONE " << (untouched ? 1 : 0) << "\n";
    }

    std::cout << R"C(
static int checks = 0;

/* Stand-in dispatcher: look the block up by PC, hand it a budget, run it, and
   keep going until the guest returns or nothing covers the PC. */
static int drive(GuestContext* c, void (**table)(GuestContext*), int32_t budget) {
    int dispatches = 0;
    for (int guard = 0; guard < 10000; ++guard) {
        const uint64_t index = c->pc / 4;
        if (c->pc % 4 || index >= NBLK) return dispatches;   /* returned out */
        c->chain_budget = budget;
        ++dispatches;
        table[index](c);
        if (c->halted) return dispatches;
    }
    return -1;
}

static int drive_calls(GuestContext* c, BlockFn* table, int32_t budget) {
    int dispatches = 0;
    g_lookup = table; g_lookup_n = NCALL;
    for (int guard = 0; guard < 10000; ++guard) {
        const uint64_t index = c->pc / 4;
        if (c->pc % 4 || index >= NCALL) return dispatches;
        c->chain_budget = budget;
        ++dispatches;
        table[index](c);
        if (c->halted) return dispatches;
    }
    return -1;
}

static uint64_t rng = 0x243f6a8885a308d3ULL;
static uint64_t rnd(void){ rng^=rng<<13; rng^=rng>>7; rng^=rng<<17; return rng; }

static int drive_loop(GuestContext* c, int32_t budget) {
    int dispatches = 0;
    for (int guard = 0; guard < 100000; ++guard) {
        const uint64_t index = c->pc / 4;
        if (c->pc % 4 || index >= NLOOP) return dispatches;
        c->chain_budget = budget;
        ++dispatches;
        loops[index](c);
        if (c->halted) return dispatches;
    }
    return -1;
}

int main(void) {
    int t, i;
    static const int32_t budgets[] = {1, 2, 3, 5, 128};
    if (!UNKNOWN_TARGET_LEFT_ALONE) {
        printf("FAIL: um sucessor sem bloco foi encadeado na mesma\n");
        return 1;
    }
    /* A guest loop has to keep handing control back. Chained without a limit it
       never would, and the only symptom would be a hang - so the property is
       checked directly: more iterations must mean more dispatches. */
    for (t = 0; t < (int)(sizeof budgets / sizeof budgets[0]); ++t) {
        GuestContext a, b;
        int da, db;
        /* x30 has to point outside the table, or the RET at the end sends the
           dispatcher back to block zero and the run never finishes. */
        memset(&a, 0, sizeof a); a.x[0] = 40; a.x[1] = 3; a.pc = 0; a.x[30] = 0x10000;
        memset(&b, 0, sizeof b); b.x[0] = 80; b.x[1] = 3; b.pc = 0; b.x[30] = 0x10000;
        da = drive_loop(&a, budgets[t]);
        db = drive_loop(&b, budgets[t]);
        if (da < 0 || db < 0) { printf("FAIL: ciclo nao devolveu o controlo (budget=%d)\n", budgets[t]); return 1; }
        if (db <= da) {
            printf("FAIL budget=%d: o dobro das iteracoes nao deu mais despachos (%d vs %d)\n",
                   budgets[t], db, da);
            return 1;
        }
        if (a.x[0] != 0 || b.x[0] != 0) { printf("FAIL: o ciclo nao correu ate ao fim\n"); return 1; }
        ++checks;
    }
    /* Indirect branches: same result as the plain blocks for every budget;
       with room to spare the run is one dispatch, which only happens if BLR
       and RET were chained; and the BR out of the module still returns. */
    for (t = 0; t < (int)(sizeof budgets / sizeof budgets[0]); ++t) {
        GuestContext ref, trial;
        int dref, dtrial;
        memset(&ref, 0, sizeof ref); ref.x[0] = 5; ref.x[6] = 0x14; ref.x[30] = 0x10000;
        trial = ref;
        dref = drive_calls(&ref, callplains, 1);
        dtrial = drive_calls(&trial, callchains, budgets[t]);
        if (dref < 0 || dtrial < 0) { printf("FAIL: indirect run did not finish\n"); return 1; }
        if (memcmp(trial.x, ref.x, sizeof ref.x) || trial.pc != ref.pc || trial.pc != 0x10000 ||
            trial.x[0] != 8) {
            printf("FAIL budget=%d: indirect chaining changed the result (x0=%llu pc=%llx)\n",
                   budgets[t], (unsigned long long)trial.x[0], (unsigned long long)trial.pc);
            return 1;
        }
        if (budgets[t] >= 128 && dtrial != 1) {
            printf("FAIL: BLR/RET not chained (%d dispatches)\n", dtrial);
            return 1;
        }
        if (budgets[t] == 1 && dtrial != dref) {
            printf("FAIL: budget 1 still chained an indirect branch (%d vs %d)\n", dtrial, dref);
            return 1;
        }
        ++checks;
    }
    g_lookup = 0; g_lookup_n = 0;
    for (t = 0; t < 400; ++t) {
        GuestContext base;
        uint8_t mem0[MEMSZ];
        memset(&base, 0, sizeof base);
        for (i = 0; i < 32; ++i) base.x[i] = rnd();
        base.x[3] = (rnd() % (MEMSZ / 2)) & ~(uint64_t)7;   /* endereco valido */
        base.x[1] = rnd() % 5;                              /* exercita os dois ramos */
        base.pc = 0;
        for (i = 0; i < MEMSZ; ++i) mem0[i] = (uint8_t)rnd();

        GuestContext reference = base;
        int svc_reference;
        memcpy(g_mem, mem0, MEMSZ);
        reference.chain_budget = 1;
        g_svc_calls = 0;
        const int plain_dispatches = drive(&reference, plains, 1);
        svc_reference = g_svc_calls;
        if (!svc_reference) { printf("FAIL: o programa de teste nao executou nenhum SVC\n"); return 1; }
        uint8_t reference_mem[MEMSZ];
        memcpy(reference_mem, g_mem, MEMSZ);
        if (plain_dispatches < 0) { printf("FAIL: referencia nao terminou\n"); return 1; }

        for (i = 0; i < (int)(sizeof budgets / sizeof budgets[0]); ++i) {
            GuestContext trial = base;
            memcpy(g_mem, mem0, MEMSZ);
            g_svc_calls = 0;
            const int chained_dispatches = drive(&trial, chains, budgets[i]);
            if (chained_dispatches < 0) { printf("FAIL: encadeado nao terminou\n"); return 1; }
            if (g_svc_calls != svc_reference) {
                printf("FAIL t=%d budget=%d: %d SVC em vez de %d\n", t, budgets[i],
                       g_svc_calls, svc_reference);
                return 1;
            }
            { int r;
              for (r = 0; r < 32; ++r) {
                  if (trial.x[r] != reference.x[r]) {
                      printf("FAIL t=%d budget=%d: x[%d] %llx vs %llx\n", t, budgets[i], r,
                             (unsigned long long)trial.x[r], (unsigned long long)reference.x[r]);
                      return 1;
                  }
              } }
            if (trial.pc != reference.pc || trial.n != reference.n || trial.z != reference.z ||
                trial.c != reference.c || trial.v != reference.v || trial.halted != reference.halted) {
                printf("FAIL t=%d budget=%d: pc/flags divergem\n", t, budgets[i]);
                return 1;
            }
            if (memcmp(g_mem, reference_mem, MEMSZ)) {
                printf("FAIL t=%d budget=%d: memoria diverge\n", t, budgets[i]);
                return 1;
            }
            /* A budget above one has to cut dispatches, or nothing is chained
               and the test would be passing without exercising the rewrite. */
            if (budgets[i] >= 5 && chained_dispatches >= plain_dispatches) {
                printf("FAIL t=%d: budget=%d nao reduziu despachos (%d vs %d)\n",
                       t, budgets[i], chained_dispatches, plain_dispatches);
                return 1;
            }
            ++checks;
        }
    }
    printf("%d chaining comparisons passed\n", checks);
    return 0;
}
)C";
    return 0;
}
