// SPDX-License-Identifier: GPL-2.0-or-later
// Emits an executable test of the Advanced SIMD instructions added for the
// boot path: the integer two-register misc forms, the permutes, the modified
// immediates, the shifts by immediate and FMLA/FMLS by element.
#include <iostream>
#include <string>
#include <vector>
#include "core/recompiler/arm64_to_c.h"

static const unsigned kSameOps[] = {0x00,0x02,0x04,0x06,0x07,0x08,0x09,0x0C,0x0D,
                                    0x0E,0x0F,0x11,0x12,0x14,0x15,0x17};
static bool SameShapeOk(unsigned op, unsigned size, unsigned Q) {
    if (size == 3 && !Q) return false;
    // Only the compares, the variable shift, CMTST/CMEQ and ADDP have a
    // 64-bit element form.
    const bool wide_ok = (op == 0x06 || op == 0x07 || op == 0x08 || op == 0x09 ||
                          op == 0x11 || op == 0x17);
    return !(size == 3 && !wide_ok);
}
struct FpSame { unsigned op, u, a; };
static const FpSame kFpSame[] = {
    {0x18,0,0}, {0x18,0,1},            // FMAXNM / FMINNM
    {0x1A,0,0}, {0x1A,0,1},            // FADD / FSUB
    {0x1A,1,1},                        // FABD
    {0x1B,1,0},                        // FMUL
    {0x1C,0,0}, {0x1C,1,0}, {0x1C,1,1},// FCMEQ / FCMGE / FCMGT
    {0x1E,0,0}, {0x1E,0,1},            // FMAX / FMIN
    {0x1F,1,0},                        // FDIV
    {0x19,0,0}, {0x19,0,1},            // FMLA / FMLS
};
static const unsigned kFpSameCount = sizeof kFpSame / sizeof kFpSame[0];
static const unsigned kAcrossOps[] = {0x03, 0x0A, 0x1A, 0x1B};
static const unsigned kScalarPair[][2] = {{0x0D,0},{0x0C,0},{0x0C,1},{0x0F,0},{0x0F,1}};
struct Single { unsigned opcode, S, size, esize, replicate; };
/* The lane index is not a field of the encoding: it is assembled from Q, S and
   size, differently for each element width. */
static unsigned single_index(struct Single sh, unsigned Q) {
    switch (sh.opcode >> 1) {
    case 0:  return (Q << 3) | (sh.S << 2) | sh.size;
    case 1:  return (Q << 2) | (sh.S << 1) | (sh.size >> 1);
    default: return sh.size == 0 ? ((Q << 1) | sh.S) : Q;
    }
}
static const struct Single kSingle[] = {
    {0,0,0, 8,0}, {0,1,2, 8,0}, {1,0,1, 8,0},
    {2,0,0,16,0}, {2,1,2,16,0}, {3,0,0,16,0},
    {4,0,0,32,0}, {4,1,0,32,0}, {5,0,0,32,0},
    {4,0,1,64,0}, {5,0,1,64,0},
    {6,0,0, 8,1}, {6,0,2,32,1}, {7,0,1,16,1}, {7,0,3,64,1},
};
struct Structs { unsigned opcode, rpt, selem; };
static const Structs kStructs[] = {
    {0x0,1,4}, {0x2,4,1}, {0x4,1,3}, {0x6,3,1}, {0x7,1,1}, {0x8,1,2}, {0xA,2,1},
};
static const unsigned kPairwise[][3] = {{0x1A,1,0},{0x18,1,0},{0x18,1,1},{0x1E,1,0},{0x1E,1,1}};
static const unsigned kDiff[] = {0,1,2,3,8,10,12};

static unsigned slot = 0;
static std::string dispatch;

static void emit(uint32_t insn) {
    std::string body;
    if (!suyu::recomp::Translate(insn, 0, body) ||
        body.find("recomp_unhandled") != std::string::npos) {
        std::cerr << "not translated: " << std::hex << insn << '\n';
        std::exit(1);
    }
    std::cout << "static void f" << slot << "(GuestContext* c){" << body << "}\n";
    dispatch += "f" + std::to_string(slot) + ",";
    ++slot;
}

int main() {
    std::cout << R"C(
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
typedef struct GuestContext { uint64_t x[32]; uint64_t vreg[32][2]; } GuestContext;
/* A flat stand-in for guest memory, so the structure loads and stores can be
   exercised without the emulator underneath. */
static uint8_t MEM[4096];
uint64_t recomp_load8(GuestContext* c,uint64_t a){ (void)c; return MEM[a]; }
uint64_t recomp_load16(GuestContext* c,uint64_t a){ uint16_t v; (void)c; memcpy(&v,MEM+a,2); return v; }
uint64_t recomp_load32(GuestContext* c,uint64_t a){ uint32_t v; (void)c; memcpy(&v,MEM+a,4); return v; }
uint64_t recomp_load64(GuestContext* c,uint64_t a){ uint64_t v; (void)c; memcpy(&v,MEM+a,8); return v; }
void recomp_store8(GuestContext* c,uint64_t a,uint64_t v){ (void)c; MEM[a]=(uint8_t)v; }
void recomp_store16(GuestContext* c,uint64_t a,uint64_t v){ uint16_t t=(uint16_t)v; (void)c; memcpy(MEM+a,&t,2); }
void recomp_store32(GuestContext* c,uint64_t a,uint64_t v){ uint32_t t=(uint32_t)v; (void)c; memcpy(MEM+a,&t,4); }
void recomp_store64(GuestContext* c,uint64_t a,uint64_t v){ (void)c; memcpy(MEM+a,&v,8); }
struct Structs { unsigned opcode, rpt, selem; };
static const struct Structs kStructs[] = {
    {0x0,1,4}, {0x2,4,1}, {0x4,1,3}, {0x6,3,1}, {0x7,1,1}, {0x8,1,2}, {0xA,2,1},
};
static const unsigned kPairwise[][3] = {{0x1A,1,0},{0x18,1,0},{0x18,1,1},{0x1E,1,0},{0x1E,1,1}};
static const unsigned kDiff[] = {0,1,2,3,8,10,12};
static const unsigned kSameOps[] = {0x00,0x02,0x04,0x06,0x07,0x08,0x09,0x0C,0x0D,
                                    0x0E,0x0F,0x11,0x12,0x14,0x15,0x17};
static int SameShapeOk(unsigned op, unsigned size, unsigned Q) {
    if (size == 3 && !Q) return 0;
    const int wide_ok = (op == 0x06 || op == 0x07 || op == 0x08 || op == 0x09 ||
                         op == 0x11 || op == 0x17);
    return !(size == 3 && !wide_ok);
}
struct FpSame { unsigned op, u, a; };
static const struct FpSame kFpSame[] = {
    {0x18,0,0}, {0x18,0,1}, {0x1A,0,0}, {0x1A,0,1}, {0x1A,1,1}, {0x1B,1,0},
    {0x1C,0,0}, {0x1C,1,0}, {0x1C,1,1}, {0x1E,0,0}, {0x1E,0,1}, {0x1F,1,0},
    {0x19,0,0}, {0x19,0,1},
};
static const unsigned kAcrossOps[] = {0x03, 0x0A, 0x1A, 0x1B};
static const unsigned kScalarPair[][2] = {{0x0D,0},{0x0C,0},{0x0C,1},{0x0F,0},{0x0F,1}};
struct Single { unsigned opcode, S, size, esize, replicate; };
/* The lane index is not a field of the encoding: it is assembled from Q, S and
   size, differently for each element width. */
static unsigned single_index(struct Single sh, unsigned Q) {
    switch (sh.opcode >> 1) {
    case 0:  return (Q << 3) | (sh.S << 2) | sh.size;
    case 1:  return (Q << 2) | (sh.S << 1) | (sh.size >> 1);
    default: return sh.size == 0 ? ((Q << 1) | sh.S) : Q;
    }
}
static const struct Single kSingle[] = {
    {0,0,0, 8,0}, {0,1,2, 8,0}, {1,0,1, 8,0},
    {2,0,0,16,0}, {2,1,2,16,0}, {3,0,0,16,0},
    {4,0,0,32,0}, {4,1,0,32,0}, {5,0,0,32,0},
    {4,0,1,64,0}, {5,0,1,64,0},
    {6,0,0, 8,1}, {6,0,2,32,1}, {7,0,1,16,1}, {7,0,3,64,1},
};
)C";

    // Two-register misc: CNT/NOT (size 0 only) and ABS/NEG (all sizes).
    for (unsigned Q = 0; Q < 2; ++Q) {
        for (unsigned U = 0; U < 2; ++U)
            emit(0x0E200800u | (Q << 30) | (U << 29) | (0x05u << 12) | (1u << 5) | 0u);
        for (unsigned U = 0; U < 2; ++U)
            for (unsigned size = 0; size < 4; ++size) {
                if (size == 3 && !Q) continue;
                emit(0x0E200800u | (Q << 30) | (U << 29) | (size << 22) |
                     (0x0Bu << 12) | (1u << 5) | 0u);
            }
    }

    // Permute.
    static const unsigned perm[] = {1, 2, 3, 5, 6, 7};
    for (unsigned Q = 0; Q < 2; ++Q)
        for (unsigned size = 0; size < 4; ++size) {
            if (size == 3 && !Q) continue;
            for (unsigned op : perm)
                emit(0x0E000800u | (Q << 30) | (size << 22) | (2u << 16) |
                     (op << 12) | (1u << 5) | 0u);
        }

    // Modified immediate.
    static const unsigned imm8s[] = {0x00, 0x01, 0x5A, 0xFF};
    for (unsigned Q = 0; Q < 2; ++Q)
        for (unsigned op = 0; op < 2; ++op)
            for (unsigned cmode = 0; cmode < 16; ++cmode) {
                if (cmode == 15 && op == 1 && !Q) continue;   // needs the full width
                for (unsigned imm8 : imm8s)
                    emit(0x0F000400u | (Q << 30) | (op << 29) | ((imm8 >> 5) << 16) |
                         (cmode << 12) | ((imm8 & 0x1F) << 5) | 0u);
            }

    // Shift by immediate. imm = immh:immb always lies in [esize, 2*esize-1].
    for (unsigned Q = 0; Q < 2; ++Q)
        for (unsigned U = 0; U < 2; ++U)
            for (unsigned size = 0; size < 4; ++size) {
                const unsigned esize = 8u << size;
                for (unsigned imm = esize; imm < 2 * esize; imm += (esize > 8 ? esize / 4 : 3)) {
                    const unsigned immh = imm >> 3, immb = imm & 7;
                    if (size < 3) {   // SSHLL/USHLL widen, so no 64-bit source
                        emit(0x0F000400u | (Q << 30) | (U << 29) | (immh << 19) |
                             (immb << 16) | (0x14u << 11) | (1u << 5) | 0u);
                    }
                    if (!U && size < 3) {   // SHRN and RSHRN narrow
                        emit(0x0F000400u | (Q << 30) | (immh << 19) | (immb << 16) |
                             (0x10u << 11) | (1u << 5) | 0u);
                        emit(0x0F000400u | (Q << 30) | (immh << 19) | (immb << 16) |
                             (0x11u << 11) | (1u << 5) | 0u);
                    }
                    if (size == 3 && !Q) continue;
                    emit(0x0F000400u | (Q << 30) | (U << 29) | (immh << 19) |
                         (immb << 16) | (0x00u << 11) | (1u << 5) | 0u);   // SSHR/USHR
                    emit(0x0F000400u | (Q << 30) | (U << 29) | (immh << 19) |
                         (immb << 16) | (0x02u << 11) | (1u << 5) | 0u);   // SSRA/USRA
                    if (!U) {
                        emit(0x0F000400u | (Q << 30) | (immh << 19) | (immb << 16) |
                             (0x0Au << 11) | (1u << 5) | 0u);              // SHL
                    }
                }
            }

    // FMLA / FMLS by element. Rm is four bits here, so keep it under 16.
    for (unsigned Q = 0; Q < 2; ++Q)
        for (unsigned dbl = 0; dbl < 2; ++dbl) {
            if (dbl && !Q) continue;      // 2D needs the full width
            for (unsigned sub = 0; sub < 2; ++sub)
                for (unsigned idx = 0; idx < (dbl ? 2u : 4u); ++idx) {
                    const unsigned L = dbl ? 0u : (idx & 1), H = dbl ? idx : (idx >> 1);
                    emit(0x0F800000u | (Q << 30) | (dbl << 22) | (L << 21) |
                         (2u << 16) | ((sub ? 5u : 1u) << 12) | (H << 11) |
                         (1u << 5) | 0u);
                }
        }

    // Three-register same, integer. The shape rules differ per opcode: the
    // halving, absolute-difference, multiply-accumulate, max/min and pairwise
    // max/min forms have no 64-bit element encoding.
    for (unsigned Q = 0; Q < 2; ++Q)
        for (unsigned U = 0; U < 2; ++U)
            for (unsigned size = 0; size < 4; ++size)
                for (unsigned op : kSameOps) {
                    if (!SameShapeOk(op, size, Q)) continue;
                    emit(0x0E200400u | (Q << 30) | (U << 29) | (size << 22) |
                         (2u << 16) | (op << 11) | (1u << 5) | 0u);
                }

    // BIT and BIF, the two logical forms that were previously left out.
    for (unsigned Q = 0; Q < 2; ++Q)
        for (unsigned size = 2; size < 4; ++size)
            emit(0x0E200400u | (Q << 30) | (1u << 29) | (size << 22) | (2u << 16) |
                 (0x03u << 11) | (1u << 5) | 0u);

    // Three-register same, floating point. `a` is bit 23 and `sz` is bit 22.
    for (unsigned Q = 0; Q < 2; ++Q)
        for (unsigned sz = 0; sz < 2; ++sz) {
            if (sz && !Q) continue;              // 2D needs the full width
            for (unsigned k = 0; k < kFpSameCount; ++k) {
                const FpSame& e = kFpSame[k];
                emit(0x0E200400u | (Q << 30) | (e.u << 29) | (e.a << 23) | (sz << 22) |
                     (2u << 16) | (e.op << 11) | (1u << 5) | 0u);
            }
        }

    // Across lanes.
    for (unsigned Q = 0; Q < 2; ++Q)
        for (unsigned U = 0; U < 2; ++U)
            for (unsigned size = 0; size < 3; ++size) {
                if (size == 2 && !Q) continue;
                for (unsigned op : kAcrossOps)
                    emit(0x0E300800u | (Q << 30) | (U << 29) | (size << 22) |
                         (op << 12) | (1u << 5) | 0u);
            }

    // EXT and TBL/TBX.
    for (unsigned Q = 0; Q < 2; ++Q) {
        const unsigned width = Q ? 16u : 8u;
        for (unsigned imm4 = 0; imm4 < width; ++imm4)
            emit(0x2E000000u | (Q << 30) | (2u << 16) | (imm4 << 11) | (1u << 5) | 0u);
        for (unsigned len = 0; len < 4; ++len)
            for (unsigned op = 0; op < 2; ++op)
                emit(0x0E000000u | (Q << 30) | (8u << 16) | (len << 13) | (op << 12) |
                     (1u << 5) | 0u);
    }

    // FP pairwise, and the structure loads and stores in both forms.
    for (unsigned Q = 0; Q < 2; ++Q)
        for (unsigned sz = 0; sz < 2; ++sz) {
            if (sz && !Q) continue;
            for (auto& e : kPairwise)
                emit(0x0E200400u | (Q << 30) | (e[1] << 29) | (e[2] << 23) | (sz << 22) |
                     (2u << 16) | (e[0] << 11) | (1u << 5) | 0u);
        }
    for (unsigned Q = 0; Q < 2; ++Q)
        for (unsigned L = 0; L < 2; ++L)
            for (unsigned post = 0; post < 2; ++post)
                for (auto& e : kStructs)
                    for (unsigned size = 0; size < 4; ++size) {
                        if (size == 3 && !Q && e.selem != 1) continue;
                        emit(0x0C000000u | (Q << 30) | (post << 23) | (L << 22) |
                             (post ? (31u << 16) : 0u) | (e.opcode << 12) | (size << 10) |
                             (0u << 5) | 1u);
                    }

    // Two-register misc: the vector FABS/FNEG, the reversals, CLS/CLZ, the
    // compares against zero and the narrowing extracts.
    for (unsigned Q = 0; Q < 2; ++Q) {
        for (unsigned U = 0; U < 2; ++U)
            for (unsigned sz = 0; sz < 2; ++sz) {
                if (sz && !Q) continue;
                emit(0x0EA0F800u | (Q << 30) | (U << 29) | (sz << 22) | (1u << 5) | 0u);
            }
        // FSQRT shares the group at opcode 11111 with U set.
        for (unsigned sz = 0; sz < 2; ++sz) {
            if (sz && !Q) continue;
            emit(0x0EA1F800u | (Q << 30) | (1u << 29) | (sz << 22) | (1u << 5) | 0u);
        }
        // REV64 (U=0), REV16 (U=0, opcode 1) and REV32 (U=1, opcode 1).
        for (unsigned size = 0; size < 3; ++size)
            emit(0x0E200800u | (Q << 30) | (0x00u << 12) | (size << 22) | (1u << 5) | 0u);
        emit(0x0E200800u | (Q << 30) | (0x01u << 12) | (0u << 22) | (1u << 5) | 0u);
        // REV32 partilha o opcode 00000 com REV64 e distingue-se por U=1;
        // o opcode 00001 e REV16, que so existe com U=0.
        for (unsigned size = 0; size < 2; ++size)
            emit(0x0E200800u | (Q << 30) | (1u << 29) | (0x00u << 12) | (size << 22) |
                 (1u << 5) | 0u);
        for (unsigned U = 0; U < 2; ++U)
            for (unsigned size = 0; size < 3; ++size)
                emit(0x0E200800u | (Q << 30) | (U << 29) | (0x04u << 12) | (size << 22) |
                     (1u << 5) | 0u);
        for (unsigned k = 0; k < 5; ++k)
            for (unsigned size = 0; size < 4; ++size) {
                if (size == 3 && !Q) continue;
                static const unsigned zc[5][2] = {{0x08,0},{0x08,1},{0x09,0},{0x09,1},{0x0A,0}};
                emit(0x0E200800u | (Q << 30) | (zc[k][1] << 29) | (zc[k][0] << 12) |
                     (size << 22) | (1u << 5) | 0u);
            }
        for (unsigned U = 0; U < 2; ++U)
            for (unsigned op : {0x12u, 0x14u})
                for (unsigned size = 0; size < 3; ++size)
                    emit(0x0E200800u | (Q << 30) | (U << 29) | (op << 12) | (size << 22) |
                         (1u << 5) | 0u);
    }

    // Scalar pairwise: FADDP, FMAXNMP/FMINNMP, FMAXP/FMINP and the integer ADDP.
    for (unsigned sz = 0; sz < 2; ++sz)
        for (auto& e : kScalarPair)
            emit(0x5E300800u | (1u << 29) | (e[1] << 23) | (sz << 22) | (e[0] << 12) |
                 (1u << 5) | 0u);
    emit(0x5E300800u | (3u << 22) | (0x1Bu << 12) | (1u << 5) | 0u);   // ADDP D, V.2D

    // Vector fixed-point conversions, integer MUL/MLA/MLS by element, and the
    // single-structure loads and stores.
    for (unsigned Q = 0; Q < 2; ++Q)
        for (unsigned size = 2; size < 4; ++size) {
            if (size == 3 && !Q) continue;
            const unsigned esize = 8u << size;
            for (unsigned fb = 1; fb <= esize; fb += (esize / 2) - 1) {
                const unsigned imm = 2 * esize - fb;
                for (unsigned U = 0; U < 2; ++U)
                    for (unsigned op : {0x1Cu, 0x1Fu})
                        emit(0x0F000400u | (Q << 30) | (U << 29) | ((imm >> 3) << 19) |
                             ((imm & 7) << 16) | (op << 11) | (1u << 5) | 0u);
            }
        }
    for (unsigned Q = 0; Q < 2; ++Q)
        for (unsigned size = 1; size < 3; ++size)
            for (unsigned op : {0x0u, 0x4u, 0x8u})
                for (unsigned idx = 0; idx < 4; ++idx) {
                    const unsigned H = (size == 1) ? (idx >> 2) : (idx >> 1);
                    const unsigned L = (size == 1) ? ((idx >> 1) & 1) : (idx & 1);
                    const unsigned M = (size == 1) ? (idx & 1) : 0;
                    // MUL por elemento e U=0; MLA e MLS sao U=1.
                    const unsigned U = (op == 0x8u) ? 0u : 1u;
                    emit(0x0F000000u | (Q << 30) | (U << 29) | (size << 22) | (L << 21) |
                         (M << 20) | (2u << 16) | (op << 12) | (H << 11) | (1u << 5) | 0u);
                }
    for (unsigned Q = 0; Q < 2; ++Q)
        for (unsigned L = 0; L < 2; ++L)
            for (unsigned post = 0; post < 2; ++post)
                for (unsigned R = 0; R < 2; ++R)
                    for (auto& sh : kSingle) {
                        if (sh.replicate && !L) continue;
                        emit(0x0D000000u | (Q << 30) | (post << 23) | (L << 22) | (R << 21) |
                             (post ? (31u << 16) : 0u) | (sh.opcode << 13) | (sh.S << 12) |
                             (sh.size << 10) | (0u << 5) | 1u);
                    }

    // Grupo de alargamento, SHLL, pares longos e SQDMULH.
    for (unsigned Q = 0; Q < 2; ++Q) {
        for (unsigned U = 0; U < 2; ++U)
            for (unsigned size = 0; size < 3; ++size)
                for (unsigned op : kDiff)
                    emit(0x0E200000u | (Q << 30) | (U << 29) | (size << 22) | (2u << 16) |
                         (op << 12) | (1u << 5) | 0u);
        for (unsigned size = 0; size < 3; ++size)
            emit(0x0E200800u | (Q << 30) | (1u << 29) | (size << 22) | (0x13u << 12) |
                 (1u << 5) | 0u);                                   // SHLL
        for (unsigned U = 0; U < 2; ++U)
            for (unsigned op : {0x02u, 0x06u})
                for (unsigned size = 0; size < 3; ++size)
                    emit(0x0E200800u | (Q << 30) | (U << 29) | (size << 22) | (op << 12) |
                         (1u << 5) | 0u);                           // S/UADDLP, S/UADALP
        for (unsigned size = 1; size < 3; ++size)
            emit(0x0E200400u | (Q << 30) | (size << 22) | (2u << 16) | (0x16u << 11) |
                 (1u << 5) | 0u);                                   // SQDMULH
    }

    std::cout << "static void (*functions[])(GuestContext*) = {" << dispatch << "};\n";
    std::cout << R"C(
/* Reference models, all written over a flat byte image of the register so they
   share no code with the emitted lane loops. */
static uint8_t A[16], B[16], D[16], R[16];
static int checks = 0;

static uint64_t lane_get(const uint8_t* p, unsigned esize, unsigned i) {
    uint64_t v = 0; memcpy(&v, p + i * (esize / 8), esize / 8); return v;
}
static void lane_put(uint8_t* p, unsigned esize, unsigned i, uint64_t v) {
    memcpy(p + i * (esize / 8), &v, esize / 8);
}
static int64_t lane_sext(uint64_t v, unsigned esize) {
    return esize == 64 ? (int64_t)v
                       : (int64_t)((v ^ (UINT64_C(1) << (esize-1))) - (UINT64_C(1) << (esize-1)));
}
static void load_ctx(GuestContext* c) {
    memcpy(c->vreg[1], A, 16); memcpy(c->vreg[2], B, 16); memcpy(c->vreg[0], D, 16);
}
static int match(const GuestContext* c, int bytes) {
    uint8_t got[16]; memcpy(got, c->vreg[0], 16);
    for (int i = 0; i < bytes; ++i) if (got[i] != R[i]) return 0;
    for (int i = bytes; i < 16; ++i) if (got[i] != 0) return 0;
    return 1;
}
static void fill_inputs(unsigned seed) {
    for (int i = 0; i < 16; ++i) {
        A[i] = (uint8_t)(seed * 37 + i * 61 + 3);
        B[i] = (uint8_t)(seed * 11 + i * 29 + 200);
        D[i] = (uint8_t)(seed * 5 + i * 7 + 90);
    }
}

int main(void) {
    unsigned slot = 0;
    GuestContext c;

    /* Two-register misc. */
    for (unsigned Q = 0; Q < 2; ++Q) {
        const int bytes = Q ? 16 : 8;
        for (unsigned U = 0; U < 2; ++U, ++slot)
        for (unsigned s = 0; s < 4; ++s) {
            fill_inputs(s); memset(R, 0, 16);
            for (int i = 0; i < bytes; ++i) {
                unsigned n = 0;
                for (int b = 0; b < 8; ++b) n += (A[i] >> b) & 1;
                R[i] = U ? (uint8_t)~A[i] : (uint8_t)n;
            }
            memset(&c, 0, sizeof c); load_ctx(&c); functions[slot](&c);
            if (!match(&c, bytes)) { printf("FAIL cnt/not Q=%u U=%u\n",Q,U); return 1; }
            ++checks;
        }
        for (unsigned U = 0; U < 2; ++U)
        for (unsigned size = 0; size < 4; ++size) {
            if (size == 3 && !Q) continue;
            const unsigned esize = 8u << size;
            for (unsigned s = 0; s < 4; ++s) {
                fill_inputs(s); memset(R, 0, 16);
                for (unsigned i = 0; i < (unsigned)bytes / (esize/8); ++i) {
                    const int64_t v = lane_sext(lane_get(A, esize, i), esize);
                    const uint64_t r = U ? (uint64_t)(0 - (uint64_t)v)
                                         : (uint64_t)(v < 0 ? 0 - (uint64_t)v : (uint64_t)v);
                    lane_put(R, esize, i, r);
                }
                memset(&c, 0, sizeof c); load_ctx(&c); functions[slot](&c);
                if (!match(&c, bytes)) { printf("FAIL abs/neg Q=%u U=%u size=%u\n",Q,U,size); return 1; }
                ++checks;
            }
            ++slot;
        }
    }

    /* Permute. */
    static const unsigned perm[] = {1, 2, 3, 5, 6, 7};
    for (unsigned Q = 0; Q < 2; ++Q) {
        const int bytes = Q ? 16 : 8;
        for (unsigned size = 0; size < 4; ++size) {
            if (size == 3 && !Q) continue;
            const unsigned esize = 8u << size;
            const unsigned lanes = (unsigned)bytes / (esize/8), half = lanes / 2;
            for (unsigned k = 0; k < 6; ++k, ++slot)
            for (unsigned s = 0; s < 3; ++s) {
                fill_inputs(s); memset(R, 0, 16);
                for (unsigned i = 0; i < half; ++i) {
                    switch (perm[k]) {
                    case 1: lane_put(R,esize,i,lane_get(A,esize,2*i));
                            lane_put(R,esize,i+half,lane_get(B,esize,2*i)); break;
                    case 5: lane_put(R,esize,i,lane_get(A,esize,2*i+1));
                            lane_put(R,esize,i+half,lane_get(B,esize,2*i+1)); break;
                    case 2: lane_put(R,esize,2*i,lane_get(A,esize,2*i));
                            lane_put(R,esize,2*i+1,lane_get(B,esize,2*i)); break;
                    case 6: lane_put(R,esize,2*i,lane_get(A,esize,2*i+1));
                            lane_put(R,esize,2*i+1,lane_get(B,esize,2*i+1)); break;
                    case 3: lane_put(R,esize,2*i,lane_get(A,esize,i));
                            lane_put(R,esize,2*i+1,lane_get(B,esize,i)); break;
                    default: lane_put(R,esize,2*i,lane_get(A,esize,i+half));
                             lane_put(R,esize,2*i+1,lane_get(B,esize,i+half)); break;
                    }
                }
                memset(&c, 0, sizeof c); load_ctx(&c); functions[slot](&c);
                if (!match(&c, bytes)) { printf("FAIL perm Q=%u size=%u op=%u\n",Q,size,perm[k]); return 1; }
                ++checks;
            }
        }
    }

    /* Modified immediate. The expected 64-bit pattern is built byte by byte
       from the architecture's description rather than from a shift formula. */
    static const unsigned imm8s[] = {0x00, 0x01, 0x5A, 0xFF};
    for (unsigned Q = 0; Q < 2; ++Q)
    for (unsigned op = 0; op < 2; ++op)
    for (unsigned cmode = 0; cmode < 16; ++cmode) {
        if (cmode == 15 && op == 1 && !Q) continue;
        for (unsigned t = 0; t < 4; ++t, ++slot) {
            const unsigned imm8 = imm8s[t];
            uint8_t pat[8]; memset(pat, 0, 8);
            int invert = 0, logical = 0;
            if ((cmode & 9) == 0) {                       /* 32-bit, byte-shifted */
                const unsigned b = (cmode >> 1) & 3;
                pat[b] = (uint8_t)imm8; pat[b+4] = (uint8_t)imm8; invert = op;
            } else if ((cmode & 9) == 1 && cmode != 15) { /* ORR / BIC, 32-bit */
                const unsigned b = (cmode >> 1) & 3;
                pat[b] = (uint8_t)imm8; pat[b+4] = (uint8_t)imm8;
                invert = op; logical = 1;
            } else if ((cmode & 13) == 8) {               /* 16-bit, byte-shifted */
                const unsigned b = (cmode >> 1) & 1;
                for (int h = 0; h < 4; ++h) pat[h*2 + b] = (uint8_t)imm8;
                invert = op;
            } else if ((cmode & 13) == 9 && cmode != 15) { /* ORR / BIC, 16-bit */
                const unsigned b = (cmode >> 1) & 1;
                for (int h = 0; h < 4; ++h) pat[h*2 + b] = (uint8_t)imm8;
                invert = op; logical = 1;
            } else if ((cmode & 14) == 12) {              /* 32-bit, ones shifted in */
                const unsigned nb = 1 + (cmode & 1);
                for (int w = 0; w < 2; ++w) {
                    for (unsigned b = 0; b < nb; ++b) pat[w*4 + b] = 0xFF;
                    pat[w*4 + nb] = (uint8_t)imm8;
                }
                invert = op;
            } else if (cmode == 14 && op == 0) {
                for (int b = 0; b < 8; ++b) pat[b] = (uint8_t)imm8;
            } else if (cmode == 14 && op == 1) {
                for (int b = 0; b < 8; ++b) pat[b] = ((imm8 >> b) & 1) ? 0xFF : 0x00;
            } else {                                       /* cmode 15: FMOV */
                const unsigned sgn = (imm8 >> 7) & 1, b6 = (imm8 >> 6) & 1;
                if (op == 0) {
                    uint32_t bits = ((uint32_t)sgn << 31) |
                                    ((uint32_t)(b6 ^ 1) << 30) |
                                    (b6 ? (0x1Fu << 25) : 0u) |
                                    (((uint32_t)imm8 >> 4 & 3) << 23) |
                                    (((uint32_t)imm8 & 0xF) << 19);
                    memcpy(pat, &bits, 4); memcpy(pat + 4, &bits, 4);
                } else {
                    uint64_t bits = ((uint64_t)sgn << 63) |
                                    ((uint64_t)(b6 ^ 1) << 62) |
                                    (b6 ? (UINT64_C(0xFF) << 54) : 0u) |
                                    (((uint64_t)imm8 >> 4 & 3) << 52) |
                                    (((uint64_t)imm8 & 0xF) << 48);
                    memcpy(pat, &bits, 8);
                }
            }
            if (invert) for (int b = 0; b < 8; ++b) pat[b] = (uint8_t)~pat[b];
            memset(R, 0, 16);
            fill_inputs(t);
            if (logical) {
                /* ORR and BIC combine with the existing destination. */
                for (int b = 0; b < 8; ++b)
                    R[b] = op ? (uint8_t)(D[b] & pat[b]) : (uint8_t)(D[b] | pat[b]);
                if (Q) for (int b = 0; b < 8; ++b)
                    R[8+b] = op ? (uint8_t)(D[8+b] & pat[b]) : (uint8_t)(D[8+b] | pat[b]);
            } else {
                memcpy(R, pat, 8);
                if (Q) memcpy(R + 8, pat, 8);
            }
            memset(&c, 0, sizeof c); load_ctx(&c); functions[slot](&c);
            if (!match(&c, 16)) { printf("FAIL movi Q=%u op=%u cmode=%u imm=%x\n",Q,op,cmode,imm8); return 1; }
            ++checks;
        }
    }

    /* Shift by immediate. */
    for (unsigned Q = 0; Q < 2; ++Q) {
        const int bytes = Q ? 16 : 8;
        for (unsigned U = 0; U < 2; ++U)
        for (unsigned size = 0; size < 4; ++size) {
            const unsigned esize = 8u << size;
            for (unsigned imm = esize; imm < 2 * esize; imm += (esize > 8 ? esize / 4 : 3)) {
                if (size < 3) {                       /* SSHLL / USHLL */
                    const unsigned shift = imm - esize;
                    for (unsigned s = 0; s < 3; ++s) {
                        fill_inputs(s); memset(R, 0, 16);
                        for (unsigned i = 0; i < 8u / (esize/8); ++i) {
                            const uint64_t raw = lane_get(A + (Q ? 8 : 0), esize, i);
                            const uint64_t wide = U ? raw : (uint64_t)lane_sext(raw, esize);
                            lane_put(R, esize * 2, i, wide << shift);
                        }
                        memset(&c, 0, sizeof c); load_ctx(&c); functions[slot](&c);
                        if (!match(&c, 16)) { printf("FAIL sshll Q=%u U=%u size=%u imm=%u\n",Q,U,size,imm); return 1; }
                        ++checks;
                    }
                    ++slot;
                }
                if (!U && size < 3) {      /* SHRN and RSHRN */
                    const unsigned nshift = 2 * esize - imm;
                    for (unsigned rnd = 0; rnd < 2; ++rnd) {
                        for (unsigned s = 0; s < 3; ++s) {
                            fill_inputs(s); memset(R, 0, 16);
                            if (Q) memcpy(R, D, 8);
                            for (unsigned i = 0; i < 8u / (esize/8); ++i) {
                                const uint64_t src = lane_get(A, esize * 2, i);
                                const uint64_t bias = rnd ? (UINT64_C(1) << (nshift - 1)) : 0;
                                lane_put(R + (Q ? 8 : 0), esize, i, (src + bias) >> nshift);
                            }
                            memset(&c, 0, sizeof c); load_ctx(&c); functions[slot](&c);
                            if (!match(&c, Q ? 16 : 8)) {
                                printf("FAIL shrn Q=%u size=%u imm=%u rnd=%u\n",Q,size,imm,rnd); return 1; }
                            ++checks;
                        }
                        ++slot;
                    }
                }
                if (size == 3 && !Q) continue;
                const unsigned rshift = 2 * esize - imm;
                for (unsigned s = 0; s < 3; ++s) {     /* SSHR / USHR */
                    fill_inputs(s); memset(R, 0, 16);
                    for (unsigned i = 0; i < (unsigned)bytes / (esize/8); ++i) {
                        const uint64_t raw = lane_get(A, esize, i);
                        uint64_t r;
                        if (U) r = rshift >= esize ? 0 : (raw >> rshift);
                        else {
                            const int64_t sv = lane_sext(raw, esize);
                            r = (uint64_t)(sv >> (rshift >= esize ? esize - 1 : rshift));
                        }
                        lane_put(R, esize, i, r);
                    }
                    memset(&c, 0, sizeof c); load_ctx(&c); functions[slot](&c);
                    if (!match(&c, bytes)) { printf("FAIL shr Q=%u U=%u size=%u imm=%u\n",Q,U,size,imm); return 1; }
                    ++checks;
                }
                ++slot;
                for (unsigned s = 0; s < 3; ++s) {     /* SSRA / USRA */
                    fill_inputs(s); memset(R, 0, 16);
                    for (unsigned i = 0; i < (unsigned)bytes / (esize/8); ++i) {
                        const uint64_t raw = lane_get(A, esize, i);
                        const uint64_t acc = lane_get(D, esize, i);
                        uint64_t shifted;
                        if (U) shifted = rshift >= esize ? 0 : (raw >> rshift);
                        else {
                            const int64_t sv = lane_sext(raw, esize);
                            shifted = (uint64_t)(sv >> (rshift >= esize ? esize - 1 : rshift));
                        }
                        lane_put(R, esize, i, acc + shifted);
                    }
                    memset(&c, 0, sizeof c); load_ctx(&c); functions[slot](&c);
                    if (!match(&c, bytes)) { printf("FAIL ssra Q=%u U=%u size=%u imm=%u\n",Q,U,size,imm); return 1; }
                    ++checks;
                }
                ++slot;
                if (!U) {                              /* SHL */
                    const unsigned lshift = imm - esize;
                    for (unsigned s = 0; s < 3; ++s) {
                        fill_inputs(s); memset(R, 0, 16);
                        for (unsigned i = 0; i < (unsigned)bytes / (esize/8); ++i)
                            lane_put(R, esize, i, lane_get(A, esize, i) << lshift);
                        memset(&c, 0, sizeof c); load_ctx(&c); functions[slot](&c);
                        if (!match(&c, bytes)) { printf("FAIL shl Q=%u size=%u imm=%u\n",Q,size,imm); return 1; }
                        ++checks;
                    }
                    ++slot;
                }
            }
        }
    }

    /* FMLA / FMLS by element. Small exact values isolate the lane and index
       mapping from any question of rounding. */
    for (unsigned Q = 0; Q < 2; ++Q)
    for (unsigned dbl = 0; dbl < 2; ++dbl) {
        if (dbl && !Q) continue;
        const int bytes = Q ? 16 : 8;
        const int fsz = dbl ? 8 : 4;
        const int lanes = bytes / fsz;
        for (unsigned sub = 0; sub < 2; ++sub)
        for (unsigned idx = 0; idx < (dbl ? 2u : 4u); ++idx, ++slot) {
            memset(&c, 0, sizeof c);
            double n[4], m[4], d[4], want[4];
            for (int i = 0; i < lanes; ++i) { n[i] = i + 1; d[i] = 10 * (i + 1); }
            for (int i = 0; i < 4; ++i) m[i] = 100 + i;
            for (int i = 0; i < lanes; ++i) {
                if (dbl) { double v = n[i]; memcpy((uint8_t*)c.vreg[1]+i*8,&v,8);
                           v = d[i]; memcpy((uint8_t*)c.vreg[0]+i*8,&v,8); }
                else { float v = (float)n[i]; memcpy((uint8_t*)c.vreg[1]+i*4,&v,4);
                       v = (float)d[i]; memcpy((uint8_t*)c.vreg[0]+i*4,&v,4); }
            }
            for (int i = 0; i < (dbl ? 2 : 4); ++i) {
                if (dbl) { double v = m[i]; memcpy((uint8_t*)c.vreg[2]+i*8,&v,8); }
                else { float v = (float)m[i]; memcpy((uint8_t*)c.vreg[2]+i*4,&v,4); }
            }
            for (int i = 0; i < lanes; ++i)
                want[i] = sub ? d[i] - n[i] * m[idx] : d[i] + n[i] * m[idx];
            functions[slot](&c);
            for (int i = 0; i < lanes; ++i) {
                double got;
                if (dbl) memcpy(&got,(uint8_t*)c.vreg[0]+i*8,8);
                else { float f; memcpy(&f,(uint8_t*)c.vreg[0]+i*4,4); got = f; }
                if (got != want[i]) { printf("FAIL fmla Q=%u dbl=%u sub=%u idx=%u lane=%d got=%f want=%f\n",Q,dbl,sub,idx,i,got,want[i]); return 1; }
            }
            if (!Q) { uint8_t hi[8]; memcpy(hi,(uint8_t*)c.vreg[0]+8,8);
                      for (int i=0;i<8;++i) if (hi[i]) { printf("FAIL fmla upper\n"); return 1; } }
            ++checks;
        }
    }

    /* Three-register same, integer. */
    for (unsigned Q = 0; Q < 2; ++Q) {
        const int bytes = Q ? 16 : 8;
        for (unsigned U = 0; U < 2; ++U)
        for (unsigned size = 0; size < 4; ++size)
        for (unsigned oi = 0; oi < sizeof kSameOps / sizeof kSameOps[0]; ++oi) {
            const unsigned op = kSameOps[oi];
            if (!SameShapeOk(op, size, Q)) continue;
            const unsigned esize = 8u << size;
            const unsigned lanes = (unsigned)bytes / (esize/8);
            const uint64_t emask = esize == 64 ? UINT64_MAX : ((UINT64_C(1) << esize) - 1);
            for (unsigned s = 0; s < 3; ++s) {
                fill_inputs(s); memset(R, 0, 16);
                for (unsigned i = 0; i < lanes; ++i) {
                    const uint64_t ua = lane_get(A, esize, i), ub = lane_get(B, esize, i);
                    const uint64_t ud = lane_get(D, esize, i);
                    const __int128 ia = lane_sext(ua, esize), ib = lane_sext(ub, esize);
                    uint64_t r = 0;
                    switch (op) {
                    case 0x00: r = U ? (uint64_t)(((__uint128_t)ua + ub) >> 1)
                                     : (uint64_t)((ia + ib) >> 1); break;
                    case 0x02: r = U ? (uint64_t)(((__uint128_t)ua + ub + 1) >> 1)
                                     : (uint64_t)((ia + ib + 1) >> 1); break;
                    case 0x04: r = U ? (uint64_t)(((__int128)ua - (__int128)ub) >> 1)
                                     : (uint64_t)((ia - ib) >> 1); break;
                    case 0x06: r = (U ? (ua > ub) : (ia > ib)) ? emask : 0; break;
                    case 0x07: r = (U ? (ua >= ub) : (ia >= ib)) ? emask : 0; break;
                    case 0x08: {
                        const int sh = (int)(int8_t)(uint8_t)(ub & 0xFF);
                        if (sh >= (int)esize) r = 0;
                        else if (sh >= 0) r = ua << sh;
                        else if (-sh >= (int)esize) r = U ? 0 : (uint64_t)(ia < 0 ? -1 : 0);
                        else r = U ? (ua & emask) >> (-sh) : (uint64_t)(ia >> (-sh));
                        break;
                    }
                    case 0x09: {   /* SQSHL / UQSHL, saturating shift by register */
                        const int sh = (int)(int8_t)(uint8_t)(ub & 0xFF);
                        const __int128 top = (__int128)1 << esize;
                        const __int128 smax = (top >> 1) - 1, smin = -(top >> 1);
                        if (U) {
                            if (sh >= (int)esize) r = ua ? emask : 0;
                            else if (sh > 0) {
                                const __uint128_t t = (__uint128_t)ua << sh;
                                r = t > (__uint128_t)emask ? emask : (uint64_t)t;
                            } else if (sh == 0) r = ua;
                            else if (-sh >= (int)esize) r = 0;
                            else r = (ua & emask) >> (-sh);
                        } else {
                            if (sh >= (int)esize) r = ia ? (uint64_t)(ia > 0 ? smax : smin) : 0;
                            else if (sh > 0) {
                                /* A left shift of a negative value is undefined,
                                   so scale by multiplication instead. */
                                const __int128 t = ia * ((__int128)1 << sh);
                                r = (uint64_t)(t > smax ? smax : (t < smin ? smin : t));
                            } else if (sh == 0) r = ua;
                            else if (-sh >= (int)esize) r = (uint64_t)(ia < 0 ? -1 : 0);
                            else r = (uint64_t)(ia >> (-sh));
                        }
                        break;
                    }
                    case 0x0C: r = U ? (ua > ub ? ua : ub) : (ia > ib ? ua : ub); break;
                    case 0x0D: r = U ? (ua < ub ? ua : ub) : (ia < ib ? ua : ub); break;
                    case 0x0E: r = U ? (ua > ub ? ua - ub : ub - ua)
                                     : (uint64_t)(ia > ib ? ia - ib : ib - ia); break;
                    case 0x0F: r = ud + (U ? (ua > ub ? ua - ub : ub - ua)
                                           : (uint64_t)(ia > ib ? ia - ib : ib - ia)); break;
                    case 0x11: r = (U ? (ua == ub) : ((ua & ub) != 0)) ? emask : 0; break;
                    case 0x12: r = U ? ud - ua * ub : ud + ua * ub; break;
                    default: break;   /* pairwise handled below */
                    }
                    if (op == 0x14 || op == 0x15 || op == 0x17) {
                        /* Pairwise: Rn supplies the lower result lanes, Rm the
                           upper, each result lane folding one adjacent pair. */
                        const uint8_t* src = i < lanes/2 ? A : B;
                        const unsigned j = (i < lanes/2 ? i : i - lanes/2) * 2;
                        const uint64_t pa = lane_get(src, esize, j), pb = lane_get(src, esize, j+1);
                        const __int128 spa = lane_sext(pa, esize), spb = lane_sext(pb, esize);
                        if (op == 0x17) r = pa + pb;
                        else if (op == 0x14) r = U ? (pa > pb ? pa : pb) : (spa > spb ? pa : pb);
                        else r = U ? (pa < pb ? pa : pb) : (spa < spb ? pa : pb);
                    }
                    lane_put(R, esize, i, r & emask);
                }
                memset(&c, 0, sizeof c); load_ctx(&c); functions[slot](&c);
                if (!match(&c, bytes)) { printf("FAIL same Q=%u U=%u size=%u op=%02x\n",Q,U,size,op); return 1; }
                ++checks;
            }
            ++slot;
        }
    }

    /* BIT and BIF. */
    for (unsigned Q = 0; Q < 2; ++Q) {
        const int bytes = Q ? 16 : 8;
        for (unsigned size = 2; size < 4; ++size, ++slot)
        for (unsigned s = 0; s < 3; ++s) {
            fill_inputs(s); memset(R, 0, 16);
            for (int i = 0; i < bytes; ++i) {
                const uint8_t sel = size == 2 ? B[i] : (uint8_t)~B[i];
                R[i] = (uint8_t)((D[i] & ~sel) | (A[i] & sel));
            }
            memset(&c, 0, sizeof c); load_ctx(&c); functions[slot](&c);
            if (!match(&c, bytes)) { printf("FAIL bit/bif Q=%u size=%u\n",Q,size); return 1; }
            ++checks;
        }
    }

    /* Three-register same, floating point. Values are small and exact, plus a
       NaN operand, which is where FMAX and FMAXNM part company. */
    for (unsigned Q = 0; Q < 2; ++Q)
    for (unsigned sz = 0; sz < 2; ++sz) {
        if (sz && !Q) continue;
        const int bytes = Q ? 16 : 8;
        const int fsz = sz ? 8 : 4;
        const int lanes = bytes / fsz;
        for (unsigned k = 0; k < sizeof kFpSame / sizeof kFpSame[0]; ++k, ++slot) {
            const unsigned op = kFpSame[k].op, U = kFpSame[k].u, a = kFpSame[k].a;
            for (unsigned t = 0; t < 3; ++t) {
                memset(&c, 0, sizeof c);
                double va[4], vb[4], vd[4];
                for (int i = 0; i < lanes; ++i) {
                    va[i] = (i + 1) * 2.0; vb[i] = (i + 1) * 0.5 - 1.0; vd[i] = i + 3.0;
                }
                if (t == 1) vb[0] = va[0];               /* an equal pair */
                const int nan_lane = (t == 2) ? 0 : -1;
                for (int i = 0; i < lanes; ++i) {
                    double an = (i == nan_lane) ? NAN : va[i];
                    if (sz) { memcpy((uint8_t*)c.vreg[1]+i*8,&an,8);
                              memcpy((uint8_t*)c.vreg[2]+i*8,&vb[i],8);
                              memcpy((uint8_t*)c.vreg[0]+i*8,&vd[i],8); }
                    else { float f=(float)an; memcpy((uint8_t*)c.vreg[1]+i*4,&f,4);
                           f=(float)vb[i]; memcpy((uint8_t*)c.vreg[2]+i*4,&f,4);
                           f=(float)vd[i]; memcpy((uint8_t*)c.vreg[0]+i*4,&f,4); }
                    if (i == nan_lane) va[i] = NAN;
                }
                functions[slot](&c);
                for (int i = 0; i < lanes; ++i) {
                    const double x = sz ? va[i] : (double)(float)va[i];
                    const double y = sz ? vb[i] : (double)(float)vb[i];
                    const double z = sz ? vd[i] : (double)(float)vd[i];
                    const int is_cmp = (op == 0x1C);
                    if (is_cmp) {
                        uint64_t want = 0;
                        const int res = !U ? (x == y) : (a ? (x > y) : (x >= y));
                        want = res ? (fsz == 4 ? 0xFFFFFFFFu : UINT64_MAX) : 0;
                        uint64_t got = 0; memcpy(&got,(uint8_t*)c.vreg[0]+i*fsz,fsz);
                        if (got != want) { printf("FAIL fcmp op=%02x U=%u a=%u lane=%d\n",op,U,a,i); return 1; }
                        continue;
                    }
                    double want;
                    switch (op) {
                    case 0x18: want = a ? (x != x ? y : (y != y ? x : (x < y ? x : y)))
                                        : (x != x ? y : (y != y ? x : (x > y ? x : y))); break;
                    case 0x1A: want = (U && a) ? fabs(x - y) : (a ? x - y : x + y); break;
                    case 0x1B: want = x * y; break;
                    case 0x1E: want = (x != x || y != y) ? (x + y)
                                                        : (a ? (x < y ? x : y) : (x > y ? x : y)); break;
                    case 0x1F: want = x / y; break;
                    default:   want = a ? z - x * y : z + x * y; break;   /* FMLA / FMLS */
                    }
                    if (!sz) want = (double)(float)want;
                    double got;
                    if (sz) memcpy(&got,(uint8_t*)c.vreg[0]+i*8,8);
                    else { float f; memcpy(&f,(uint8_t*)c.vreg[0]+i*4,4); got = f; }
                    const int both_nan = (got != got) && (want != want);
                    if (!both_nan && got != want) {
                        printf("FAIL fp op=%02x U=%u a=%u sz=%u lane=%d got=%f want=%f\n",op,U,a,sz,i,got,want);
                        return 1;
                    }
                }
                ++checks;
            }
        }
    }

    /* Across lanes. */
    for (unsigned Q = 0; Q < 2; ++Q) {
        const int bytes = Q ? 16 : 8;
        for (unsigned U = 0; U < 2; ++U)
        for (unsigned size = 0; size < 3; ++size) {
            if (size == 2 && !Q) continue;
            const unsigned esize = 8u << size;
            const unsigned lanes = (unsigned)bytes / (esize/8);
            const uint64_t emask = ((UINT64_C(1) << esize) - 1);
            for (unsigned oi = 0; oi < sizeof kAcrossOps / sizeof kAcrossOps[0]; ++oi, ++slot) {
                const unsigned op = kAcrossOps[oi];
                for (unsigned s = 0; s < 3; ++s) {
                    fill_inputs(s); memset(R, 0, 16);
                    __int128 acc = 0;
                    int first = 1;
                    for (unsigned i = 0; i < lanes; ++i) {
                        const uint64_t u = lane_get(A, esize, i);
                        const __int128 v = U ? (__int128)u : (__int128)lane_sext(u, esize);
                        if (op == 0x03 || op == 0x1B) acc += v;
                        else if (first) { acc = v; first = 0; }
                        else if (op == 0x0A) { if (v > acc) acc = v; }
                        else { if (v < acc) acc = v; }
                    }
                    if (op == 0x03) lane_put(R, esize * 2, 0, (uint64_t)acc &
                                             (esize * 2 == 64 ? UINT64_MAX
                                                              : ((UINT64_C(1) << (esize*2)) - 1)));
                    else lane_put(R, esize, 0, (uint64_t)acc & emask);
                    memset(&c, 0, sizeof c); load_ctx(&c); functions[slot](&c);
                    if (!match(&c, op == 0x03 ? (int)(esize*2)/8 : (int)esize/8)) {
                        printf("FAIL across Q=%u U=%u size=%u op=%02x\n",Q,U,size,op); return 1; }
                    ++checks;
                }
            }
        }
    }

    /* EXT and TBL/TBX. The table registers are V1..V1+len, so the inputs are
       written into consecutive registers rather than the usual two. */
    for (unsigned Q = 0; Q < 2; ++Q) {
        const unsigned width = Q ? 16u : 8u;
        for (unsigned imm4 = 0; imm4 < width; ++imm4, ++slot)
        for (unsigned s = 0; s < 3; ++s) {
            fill_inputs(s); memset(R, 0, 16);
            uint8_t cat[32];
            memcpy(cat, A, width); memcpy(cat + width, B, width);
            memcpy(R, cat + imm4, width);
            memset(&c, 0, sizeof c); load_ctx(&c); functions[slot](&c);
            if (!match(&c, (int)width)) { printf("FAIL ext Q=%u imm4=%u\n",Q,imm4); return 1; }
            ++checks;
        }
        for (unsigned len = 0; len < 4; ++len)
        for (unsigned op = 0; op < 2; ++op, ++slot)
        for (unsigned s = 0; s < 3; ++s) {
            fill_inputs(s); memset(R, 0, 16);
            uint8_t table[64];
            GuestContext g; memset(&g, 0, sizeof g);
            for (unsigned k = 0; k <= len; ++k)
                for (int b = 0; b < 16; ++b) {
                    table[k*16 + b] = (uint8_t)(s * 13 + k * 31 + b * 7 + 1);
                    ((uint8_t*)g.vreg[1 + k])[b] = table[k*16 + b];
                }
            /* Indices: a spread that lands both inside and past the table. */
            for (unsigned b = 0; b < width; ++b) ((uint8_t*)g.vreg[8])[b] = (uint8_t)(b * 11 + s * 5);
            for (int b = 0; b < 16; ++b) ((uint8_t*)g.vreg[0])[b] = D[b];
            const unsigned tsize = (len + 1) * 16;
            for (unsigned b = 0; b < width; ++b) {
                const uint8_t idx = ((uint8_t*)g.vreg[8])[b];
                R[b] = idx < tsize ? table[idx] : (op ? D[b] : 0);
            }
            c = g; functions[slot](&c);
            if (!match(&c, (int)width)) { printf("FAIL tbl Q=%u len=%u op=%u\n",Q,len,op); return 1; }
            ++checks;
        }
    }

    /* FP pairwise. Each result lane folds an adjacent pair of the
       concatenation Vn:Vm. */
    for (unsigned Q = 0; Q < 2; ++Q)
    for (unsigned sz = 0; sz < 2; ++sz) {
        if (sz && !Q) continue;
        const int bytes = Q ? 16 : 8;
        const int fsz = sz ? 8 : 4;
        const int lanes = bytes / fsz;
        for (unsigned k = 0; k < 5; ++k, ++slot) {
            const unsigned op = kPairwise[k][0], a = kPairwise[k][2];
            memset(&c, 0, sizeof c);
            double n[4], m[4], cat[8];
            for (int i = 0; i < lanes; ++i) { n[i] = i * 3.0 - 2.0; m[i] = 5.0 - i; }
            for (int i = 0; i < lanes; ++i) {
                if (sz) { memcpy((uint8_t*)c.vreg[1]+i*8,&n[i],8); memcpy((uint8_t*)c.vreg[2]+i*8,&m[i],8); }
                else { float f=(float)n[i]; memcpy((uint8_t*)c.vreg[1]+i*4,&f,4);
                       f=(float)m[i]; memcpy((uint8_t*)c.vreg[2]+i*4,&f,4); }
                cat[i] = n[i]; cat[lanes+i] = m[i];
            }
            functions[slot](&c);
            for (int i = 0; i < lanes; ++i) {
                const double x = cat[2*i], y = cat[2*i+1];
                double want = (op == 0x1A) ? x + y : (a ? (x < y ? x : y) : (x > y ? x : y));
                if (!sz) want = (double)(float)want;
                double got;
                if (sz) memcpy(&got,(uint8_t*)c.vreg[0]+i*8,8);
                else { float f; memcpy(&f,(uint8_t*)c.vreg[0]+i*4,4); got = f; }
                if (got != want) { printf("FAIL fpp op=%02x a=%u sz=%u lane=%d got=%f want=%f\n",op,a,sz,i,got,want); return 1; }
            }
            ++checks;
        }
    }

    /* Structure loads and stores. The reference walks the order the
       architecture specifies: registers in turn, then elements, then the
       interleaved members of each structure. */
    for (unsigned Q = 0; Q < 2; ++Q)
    for (unsigned L = 0; L < 2; ++L)
    for (unsigned post = 0; post < 2; ++post)
    for (unsigned si = 0; si < sizeof kStructs / sizeof kStructs[0]; ++si)
    for (unsigned size = 0; size < 4; ++size) {
        const unsigned rpt = kStructs[si].rpt, selem = kStructs[si].selem;
        if (size == 3 && !Q && selem != 1) continue;
        const unsigned eb = 1u << size;
        const unsigned elements = (Q ? 16u : 8u) / eb;
        const unsigned nregs = rpt * selem;
        const unsigned total = nregs * elements * eb;
        const uint64_t base = 256;
        static uint8_t want_mem[4096];
        uint8_t want_reg[4][16], src_reg[4][16];
        memset(&c, 0, sizeof c);
        for (unsigned b = 0; b < sizeof MEM; ++b) MEM[b] = (uint8_t)(b * 7 + si * 3 + size);
        memcpy(want_mem, MEM, sizeof MEM);
        for (unsigned k = 0; k < nregs; ++k)
            for (int b = 0; b < 16; ++b) {
                const uint8_t v = (uint8_t)(k * 29 + b * 13 + size * 5 + 1);
                ((uint8_t*)c.vreg[1 + k])[b] = v;
                src_reg[k][b] = v;
                /* A load leaves the upper half of a half-width register zero. */
                want_reg[k][b] = L ? 0 : v;
            }
        c.x[0] = base;
        uint64_t off = 0;
        for (unsigned r = 0; r < rpt; ++r)
            for (unsigned e = 0; e < elements; ++e)
                for (unsigned se = 0; se < selem; ++se) {
                    const unsigned k = r * selem + se;
                    if (L) memcpy(want_reg[k] + e * eb, MEM + base + off, eb);
                    else   memcpy(want_mem + base + off, src_reg[k] + e * eb, eb);
                    off += eb;
                }
        functions[slot](&c);
        if (memcmp(want_mem, MEM, sizeof MEM) != 0) {
            printf("FAIL ldst mem Q=%u L=%u post=%u op=%x size=%u\n",Q,L,post,kStructs[si].opcode,size); return 1; }
        if (L) for (unsigned k = 0; k < nregs; ++k)
            if (memcmp(want_reg[k], (uint8_t*)c.vreg[1 + k], 16) != 0) {
                printf("FAIL ldst reg Q=%u L=%u post=%u op=%x size=%u k=%u\n",Q,L,post,kStructs[si].opcode,size,k); return 1; }
        if (c.x[0] != base + (post ? total : 0)) {
            printf("FAIL ldst base Q=%u post=%u op=%x size=%u got=%llu\n",Q,post,kStructs[si].opcode,size,(unsigned long long)c.x[0]); return 1; }
        ++checks; ++slot;
    }

    /* Two-register misc: FABS/FNEG, reversals, CLS/CLZ, compares against zero
       and the narrowing extracts. */
    for (unsigned Q = 0; Q < 2; ++Q) {
        const int bytes = Q ? 16 : 8;
        for (unsigned U = 0; U < 2; ++U)
        for (unsigned sz = 0; sz < 2; ++sz) {
            if (sz && !Q) continue;
            const int fsz = sz ? 8 : 4;
            const int lanes = bytes / fsz;
            memset(&c, 0, sizeof c);
            double v[4];
            for (int i = 0; i < lanes; ++i) {
                v[i] = (i % 2 ? -1.0 : 1.0) * (i + 1) * 1.5;
                if (sz) memcpy((uint8_t*)c.vreg[1]+i*8,&v[i],8);
                else { float f=(float)v[i]; memcpy((uint8_t*)c.vreg[1]+i*4,&f,4); }
            }
            functions[slot](&c);
            for (int i = 0; i < lanes; ++i) {
                const double want = U ? -v[i] : (v[i] < 0 ? -v[i] : v[i]);
                double got;
                if (sz) memcpy(&got,(uint8_t*)c.vreg[0]+i*8,8);
                else { float f; memcpy(&f,(uint8_t*)c.vreg[0]+i*4,4); got = f; }
                if (got != want) { printf("FAIL fabsneg Q=%u U=%u sz=%u\n",Q,U,sz); return 1; }
            }
            ++checks; ++slot;
        }
        for (unsigned sz = 0; sz < 2; ++sz) {
            if (sz && !Q) continue;
            {
                const int fsz = sz ? 8 : 4;
                const int lanes = bytes / fsz;
                memset(&c, 0, sizeof c);
                double v[4];
                for (int i = 0; i < lanes; ++i) {
                    v[i] = (i + 1) * 2.25;
                    if (sz) memcpy((uint8_t*)c.vreg[1]+i*8,&v[i],8);
                    else { float f=(float)v[i]; memcpy((uint8_t*)c.vreg[1]+i*4,&f,4); }
                }
                functions[slot](&c);
                for (int i = 0; i < lanes; ++i) {
                    double want = sz ? sqrt(v[i]) : (double)sqrtf((float)v[i]);
                    double got;
                    if (sz) memcpy(&got,(uint8_t*)c.vreg[0]+i*8,8);
                    else { float f; memcpy(&f,(uint8_t*)c.vreg[0]+i*4,4); got = f; }
                    if (got != want) { printf("FAIL fsqrt Q=%u sz=%u\n",Q,sz); return 1; }
                }
                ++checks; ++slot;
            }
        }
        /* The reversals: each group of `container` bits has its elements put
           back in the opposite order. */
        for (unsigned k = 0; k < 6; ++k) {
            unsigned container, size, U;
            if (k < 3) { container = 64; size = k; U = 0; }
            else if (k == 3) { container = 16; size = 0; U = 0; }
            else { container = 32; size = k - 4; U = 1; }
            const unsigned esize = 8u << size;
            const unsigned per = container / esize;
            for (unsigned s = 0; s < 3; ++s) {
                fill_inputs(s); memset(R, 0, 16);
                const unsigned lanes = (unsigned)bytes / (esize/8);
                for (unsigned g = 0; g < lanes / per; ++g)
                    for (unsigned i = 0; i < per; ++i)
                        lane_put(R, esize, g*per + i, lane_get(A, esize, g*per + per - 1 - i));
                memset(&c, 0, sizeof c); load_ctx(&c); functions[slot](&c);
                if (!match(&c, bytes)) { printf("FAIL rev Q=%u k=%u\n",Q,k); return 1; }
                ++checks;
            }
            ++slot;
        }
        for (unsigned U = 0; U < 2; ++U)
        for (unsigned size = 0; size < 3; ++size) {
            const unsigned esize = 8u << size;
            const unsigned lanes = (unsigned)bytes / (esize/8);
            for (unsigned s = 0; s < 3; ++s) {
                fill_inputs(s); memset(R, 0, 16);
                for (unsigned i = 0; i < lanes; ++i) {
                    const uint64_t v = lane_get(A, esize, i);
                    unsigned n = 0;
                    if (U) { while (n < esize && !((v >> (esize-1-n)) & 1)) ++n; }
                    else   { while (n < esize-1 &&
                                    (((v >> (esize-1-n)) & 1) == ((v >> (esize-2-n)) & 1))) ++n; }
                    lane_put(R, esize, i, n);
                }
                memset(&c, 0, sizeof c); load_ctx(&c); functions[slot](&c);
                if (!match(&c, bytes)) { printf("FAIL cls/clz Q=%u U=%u size=%u\n",Q,U,size); return 1; }
                ++checks;
            }
            ++slot;
        }
        for (unsigned k = 0; k < 5; ++k)
        for (unsigned size = 0; size < 4; ++size) {
            if (size == 3 && !Q) continue;
            static const unsigned zc[5][2] = {{0x08,0},{0x08,1},{0x09,0},{0x09,1},{0x0A,0}};
            const unsigned op = zc[k][0], U = zc[k][1];
            const unsigned esize = 8u << size;
            const unsigned lanes = (unsigned)bytes / (esize/8);
            const uint64_t emask = esize == 64 ? UINT64_MAX : ((UINT64_C(1) << esize) - 1);
            for (unsigned s = 0; s < 3; ++s) {
                fill_inputs(s); memset(R, 0, 16);
                for (unsigned i = 0; i < lanes; ++i) {
                    const int64_t v = lane_sext(lane_get(A, esize, i), esize);
                    int res;
                    if (op == 0x08) res = U ? (v >= 0) : (v > 0);
                    else if (op == 0x09) res = U ? (v <= 0) : (v == 0);
                    else res = (v < 0);
                    lane_put(R, esize, i, res ? emask : 0);
                }
                memset(&c, 0, sizeof c); load_ctx(&c); functions[slot](&c);
                if (!match(&c, bytes)) { printf("FAIL cmpzero Q=%u k=%u size=%u\n",Q,k,size); return 1; }
                ++checks;
            }
            ++slot;
        }
        /* XTN, SQXTUN, SQXTN and UQXTN: the source lanes are twice as wide and
           Q picks which half of the destination is written. */
        for (unsigned U = 0; U < 2; ++U)
        for (unsigned oi = 0; oi < 2; ++oi)
        for (unsigned size = 0; size < 3; ++size) {
            const unsigned op = oi ? 0x14u : 0x12u;
            const unsigned esize = 8u << size, ssize = esize * 2;
            const unsigned lanes = 8u / (esize/8);
            const uint64_t dmax = (UINT64_C(1) << esize) - 1;
            const int64_t smax = (int64_t)(dmax >> 1), smin = -smax - 1;
            for (unsigned s = 0; s < 3; ++s) {
                fill_inputs(s);
                memset(R, 0, 16);
                if (Q) memcpy(R, D, 8);          /* the low half is preserved */
                for (unsigned i = 0; i < lanes; ++i) {
                    const uint64_t raw = lane_get(A, ssize, i);
                    const int64_t sv = lane_sext(raw, ssize);
                    uint64_t r;
                    if (op == 0x12 && !U) r = raw & dmax;
                    else if (op == 0x12)  r = sv < 0 ? 0 : (sv > (int64_t)dmax ? dmax : (uint64_t)sv);
                    else if (!U)          r = (uint64_t)(sv > smax ? smax : (sv < smin ? smin : sv)) & dmax;
                    else                  r = raw > dmax ? dmax : raw;
                    lane_put(R + (Q ? 8 : 0), esize, i, r);
                }
                memset(&c, 0, sizeof c); load_ctx(&c); functions[slot](&c);
                if (!match(&c, Q ? 16 : 8)) {
                    printf("FAIL xtn Q=%u U=%u op=%x size=%u\n",Q,U,op,size); return 1; }
                ++checks;
            }
            ++slot;
        }
    }

    /* Scalar pairwise. The two lanes of Vn fold into one scalar in Vd. */
    for (unsigned sz = 0; sz < 2; ++sz)
    for (unsigned k = 0; k < 5; ++k, ++slot) {
        const unsigned op = kScalarPair[k][0], a = kScalarPair[k][1];
        const int fsz = sz ? 8 : 4;
        memset(&c, 0, sizeof c);
        const double v[2] = {2.5, -1.25};
        for (int i = 0; i < 2; ++i) {
            if (sz) memcpy((uint8_t*)c.vreg[1]+i*8,&v[i],8);
            else { float f=(float)v[i]; memcpy((uint8_t*)c.vreg[1]+i*4,&f,4); }
        }
        functions[slot](&c);
        {
            const double x = sz ? v[0] : (double)(float)v[0];
            const double y = sz ? v[1] : (double)(float)v[1];
            double want;
            if (op == 0x0D) want = x + y;
            else want = a ? (x < y ? x : y) : (x > y ? x : y);
            if (!sz) want = (double)(float)want;
            double got;
            if (sz) memcpy(&got,c.vreg[0],8);
            else { float f; memcpy(&f,c.vreg[0],4); got = f; }
            if (got != want) { printf("FAIL scalarpair op=%02x a=%u sz=%u got=%f want=%f\n",op,a,sz,got,want); return 1; }
            if (c.vreg[0][1] != 0) { printf("FAIL scalarpair upper\n"); return 1; }
        }
        ++checks;
    }
    {   /* ADDP Dd, Vn.2D */
        memset(&c, 0, sizeof c);
        c.vreg[1][0] = 0x1122334455667788ULL; c.vreg[1][1] = 0x0102030405060708ULL;
        functions[slot](&c); ++slot;
        if (c.vreg[0][0] != (0x1122334455667788ULL + 0x0102030405060708ULL) || c.vreg[0][1] != 0) {
            printf("FAIL addp scalar\n"); return 1; }
        ++checks;
    }

    /* Vector conversions with a fixed-point operand. */
    for (unsigned Q = 0; Q < 2; ++Q)
    for (unsigned size = 2; size < 4; ++size) {
        if (size == 3 && !Q) continue;
        const unsigned esize = 8u << size;
        const int lanes = (Q ? 16 : 8) / (int)(esize / 8);
        for (unsigned fb = 1; fb <= esize; fb += (esize / 2) - 1)
        for (unsigned U = 0; U < 2; ++U)
        for (unsigned oi = 0; oi < 2; ++oi, ++slot) {
            const unsigned op = oi ? 0x1Fu : 0x1Cu;
            const double scale = ldexp(1.0, (int)fb);
            memset(&c, 0, sizeof c);
            static const double vals[4] = {0.0, 1.5, -2.25, 1e9};
            for (int i = 0; i < lanes; ++i) {
                if (op == 0x1C) {
                    const int64_t iv = (int64_t)(vals[i % 4] * 1000.0);
                    if (esize == 64) memcpy((uint8_t*)c.vreg[1]+i*8,&iv,8);
                    else { int32_t v32 = (int32_t)iv; memcpy((uint8_t*)c.vreg[1]+i*4,&v32,4); }
                } else {
                    if (esize == 64) memcpy((uint8_t*)c.vreg[1]+i*8,&vals[i % 4],8);
                    else { float f = (float)vals[i % 4]; memcpy((uint8_t*)c.vreg[1]+i*4,&f,4); }
                }
            }
            functions[slot](&c);
            for (int i = 0; i < lanes; ++i) {
                if (op == 0x1C) {           /* SCVTF / UCVTF */
                    double want;
                    if (esize == 64) {
                        int64_t iv; memcpy(&iv,(uint8_t*)c.vreg[1]+i*8,8);
                        want = (U ? (double)(uint64_t)iv : (double)iv) / scale;
                        double got; memcpy(&got,(uint8_t*)c.vreg[0]+i*8,8);
                        if (got != want) { printf("FAIL cvtf64 Q=%u fb=%u U=%u\n",Q,fb,U); return 1; }
                    } else {
                        int32_t iv; memcpy(&iv,(uint8_t*)c.vreg[1]+i*4,4);
                        const float wf = (U ? (float)(uint32_t)iv : (float)iv) / (float)scale;
                        float got; memcpy(&got,(uint8_t*)c.vreg[0]+i*4,4);
                        if (got != wf) { printf("FAIL cvtf32 Q=%u fb=%u U=%u\n",Q,fb,U); return 1; }
                    }
                } else {                    /* FCVTZS / FCVTZU */
                    if (esize == 64) {
                        double fv; memcpy(&fv,(uint8_t*)c.vreg[1]+i*8,8);
                        const double sv = fv * scale;
                        const double lo = U ? 0.0 : -9223372036854775808.0;
                        const double hi = U ? 18446744073709551615.0 : 9223372036854775807.0;
                        uint64_t want;
                        if (sv != sv) want = 0;
                        else if (!(sv > lo)) want = U ? 0 : 0x8000000000000000ULL;
                        else if (!(sv < hi)) want = U ? 0xFFFFFFFFFFFFFFFFULL : 0x7FFFFFFFFFFFFFFFULL;
                        else want = U ? (uint64_t)sv : (uint64_t)(int64_t)sv;
                        uint64_t got; memcpy(&got,(uint8_t*)c.vreg[0]+i*8,8);
                        if (got != want) { printf("FAIL fcvtz64 Q=%u fb=%u U=%u lane=%d\n",Q,fb,U,i); return 1; }
                    } else {
                        float fv; memcpy(&fv,(uint8_t*)c.vreg[1]+i*4,4);
                        const float sv = fv * (float)scale;
                        const float lo = U ? 0.0f : -2147483648.0f;
                        const float hi = U ? 4294967295.0f : 2147483647.0f;
                        uint32_t want;
                        if (sv != sv) want = 0;
                        else if (!(sv > lo)) want = U ? 0u : 0x80000000u;
                        else if (!(sv < hi)) want = U ? 0xFFFFFFFFu : 0x7FFFFFFFu;
                        else want = U ? (uint32_t)sv : (uint32_t)(int32_t)sv;
                        uint32_t got; memcpy(&got,(uint8_t*)c.vreg[0]+i*4,4);
                        if (got != want) { printf("FAIL fcvtz32 Q=%u fb=%u U=%u lane=%d\n",Q,fb,U,i); return 1; }
                    }
                }
            }
            ++checks;
        }
    }

    /* Integer MUL, MLA and MLS by indexed element. */
    for (unsigned Q = 0; Q < 2; ++Q)
    for (unsigned size = 1; size < 3; ++size)
    for (unsigned oi = 0; oi < 3; ++oi)
    for (unsigned idx = 0; idx < 4; ++idx, ++slot) {
        static const unsigned ops[3] = {0x0u, 0x4u, 0x8u};
        const unsigned op = ops[oi];
        const unsigned esize = 8u << size;
        const unsigned lanes = (Q ? 16u : 8u) / (esize / 8);
        const uint64_t emask = (UINT64_C(1) << esize) - 1;
        for (unsigned s = 0; s < 3; ++s) {
            fill_inputs(s); memset(R, 0, 16);
            const uint64_t m = lane_get(B, esize, idx);
            for (unsigned i = 0; i < lanes; ++i) {
                const uint64_t prod = (lane_get(A, esize, i) * m) & emask;
                const uint64_t acc = lane_get(D, esize, i);
                lane_put(R, esize, i, op == 0x8 ? prod
                                    : (op == 0x4 ? (acc - prod) : (acc + prod)));
            }
            memset(&c, 0, sizeof c); load_ctx(&c); functions[slot](&c);
            if (!match(&c, Q ? 16 : 8)) {
                printf("FAIL mulelem Q=%u size=%u op=%x idx=%u\n",Q,size,op,idx); return 1; }
            ++checks;
        }
    }

    /* Single-structure loads and stores, including the replicating forms. A
       single-lane load inserts; only the replicating forms rewrite the whole
       register. */
    for (unsigned Q = 0; Q < 2; ++Q)
    for (unsigned L = 0; L < 2; ++L)
    for (unsigned post = 0; post < 2; ++post)
    for (unsigned Rb = 0; Rb < 2; ++Rb)
    for (unsigned si = 0; si < sizeof kSingle / sizeof kSingle[0]; ++si) {
        const struct Single sh = kSingle[si];
        if (sh.replicate && !L) continue;
        const unsigned selem = (((sh.opcode & 1) << 1) | Rb) + 1;
        const unsigned eb = sh.esize / 8;
        const unsigned bytes = Q ? 16u : 8u;
        const uint64_t base = 256;
        static uint8_t want_mem[4096];
        uint8_t want_reg[4][16];
        memset(&c, 0, sizeof c);
        for (unsigned b = 0; b < sizeof MEM; ++b) MEM[b] = (uint8_t)(b * 5 + si * 7 + Rb);
        memcpy(want_mem, MEM, sizeof MEM);
        for (unsigned k = 0; k < selem; ++k)
            for (int b = 0; b < 16; ++b) {
                const uint8_t v = (uint8_t)(k * 37 + b * 11 + si * 3 + 2);
                ((uint8_t*)c.vreg[1 + k])[b] = v;
                want_reg[k][b] = v;
            }
        c.x[0] = base;
        for (unsigned k = 0; k < selem; ++k) {
            const uint64_t off = (uint64_t)k * eb;
            if (sh.replicate) {
                memset(want_reg[k], 0, 16);
                for (unsigned i = 0; i < bytes / eb; ++i)
                    memcpy(want_reg[k] + i * eb, MEM + base + off, eb);
            } else if (L) {
                memcpy(want_reg[k] + single_index(sh, Q) * eb, MEM + base + off, eb);
            } else {
                memcpy(want_mem + base + off, want_reg[k] + single_index(sh, Q) * eb, eb);
            }
        }
        functions[slot](&c);
        if (memcmp(want_mem, MEM, sizeof MEM) != 0) {
            printf("FAIL single mem Q=%u L=%u post=%u R=%u si=%u\n",Q,L,post,Rb,si); return 1; }
        if (L) for (unsigned k = 0; k < selem; ++k)
            if (memcmp(want_reg[k], (uint8_t*)c.vreg[1 + k], 16) != 0) {
                printf("FAIL single reg Q=%u L=%u post=%u R=%u si=%u k=%u\n",Q,L,post,Rb,si,k); return 1; }
        if (c.x[0] != base + (post ? (uint64_t)selem * eb : 0)) {
            printf("FAIL single base Q=%u post=%u si=%u\n",Q,post,si); return 1; }
        ++checks; ++slot;
    }

    /* Grupo de alargamento: operandos de metade da largura, Q escolhe a metade
       dos registos fonte, e as formas W usam Rn ja na largura do resultado. */
    for (unsigned Q = 0; Q < 2; ++Q) {
        for (unsigned U = 0; U < 2; ++U)
        for (unsigned size = 0; size < 3; ++size)
        for (unsigned oi = 0; oi < sizeof kDiff / sizeof kDiff[0]; ++oi, ++slot) {
            const unsigned op = kDiff[oi];
            const unsigned esize = 8u << size, wsize = esize * 2;
            const unsigned lanes = 8u / (esize / 8);
            const uint64_t wmask = wsize == 64 ? UINT64_MAX : ((UINT64_C(1) << wsize) - 1);
            const int wide = (op == 1 || op == 3), sub = (op == 2 || op == 3);
            const int mul = (op == 8 || op == 10 || op == 12);
            const int acc = (op == 8 || op == 10), msub = (op == 10);
            for (unsigned s = 0; s < 3; ++s) {
                fill_inputs(s); memset(R, 0, 16);
                for (unsigned i = 0; i < lanes; ++i) {
                    const uint64_t mraw = lane_get(B + (Q ? 8 : 0), esize, i);
                    const uint64_t m = U ? mraw : (uint64_t)lane_sext(mraw, esize) & wmask;
                    uint64_t a;
                    if (wide) a = lane_get(A, wsize, i);
                    else {
                        const uint64_t raw = lane_get(A + (Q ? 8 : 0), esize, i);
                        a = U ? raw : (uint64_t)lane_sext(raw, esize) & wmask;
                    }
                    uint64_t r;
                    if (mul) {
                        const uint64_t prod = (m * a) & wmask;
                        if (acc) { const uint64_t d = lane_get(D, wsize, i);
                                   r = (msub ? d - prod : d + prod) & wmask; }
                        else r = prod;
                    } else {
                        r = (sub ? a - m : a + m) & wmask;
                    }
                    lane_put(R, wsize, i, r);
                }
                memset(&c, 0, sizeof c); load_ctx(&c); functions[slot](&c);
                if (!match(&c, 16)) { printf("FAIL diff Q=%u U=%u size=%u op=%u\n",Q,U,size,op); return 1; }
                ++checks;
            }
        }
        /* SHLL */
        for (unsigned size = 0; size < 3; ++size, ++slot) {
            const unsigned esize = 8u << size, wsize = esize * 2;
            const unsigned lanes = 8u / (esize / 8);
            const uint64_t wmask = wsize == 64 ? UINT64_MAX : ((UINT64_C(1) << wsize) - 1);
            for (unsigned s = 0; s < 3; ++s) {
                fill_inputs(s); memset(R, 0, 16);
                for (unsigned i = 0; i < lanes; ++i)
                    lane_put(R, wsize, i, (lane_get(A + (Q ? 8 : 0), esize, i) << esize) & wmask);
                memset(&c, 0, sizeof c); load_ctx(&c); functions[slot](&c);
                if (!match(&c, 16)) { printf("FAIL shll Q=%u size=%u\n",Q,size); return 1; }
                ++checks;
            }
        }
        /* SADDLP/UADDLP e SADALP/UADALP */
        for (unsigned U = 0; U < 2; ++U)
        for (unsigned oi = 0; oi < 2; ++oi)
        for (unsigned size = 0; size < 3; ++size, ++slot) {
            const unsigned op = oi ? 0x06u : 0x02u;
            const unsigned esize = 8u << size, wsize = esize * 2;
            const unsigned bytes = Q ? 16u : 8u;
            const unsigned pairs = bytes / (esize / 8) / 2;
            const uint64_t wmask = wsize == 64 ? UINT64_MAX : ((UINT64_C(1) << wsize) - 1);
            for (unsigned s = 0; s < 3; ++s) {
                fill_inputs(s); memset(R, 0, 16);
                for (unsigned i = 0; i < pairs; ++i) {
                    const uint64_t x = lane_get(A, esize, 2*i), y = lane_get(A, esize, 2*i+1);
                    const uint64_t sx = U ? x : (uint64_t)lane_sext(x, esize);
                    const uint64_t sy = U ? y : (uint64_t)lane_sext(y, esize);
                    uint64_t r = (sx + sy) & wmask;
                    if (op == 0x06) r = (r + lane_get(D, wsize, i)) & wmask;
                    lane_put(R, wsize, i, r);
                }
                memset(&c, 0, sizeof c); load_ctx(&c); functions[slot](&c);
                if (!match(&c, (int)(pairs * wsize / 8))) {
                    printf("FAIL addlp Q=%u U=%u op=%x size=%u\n",Q,U,op,size); return 1; }
                ++checks;
            }
        }
        /* SQDMULH */
        for (unsigned size = 1; size < 3; ++size, ++slot) {
            const unsigned esize = 8u << size;
            const unsigned lanes = (Q ? 16u : 8u) / (esize / 8);
            const int64_t smax = (int64_t)((UINT64_C(1) << (esize - 1)) - 1);
            for (unsigned s = 0; s < 3; ++s) {
                fill_inputs(s); memset(R, 0, 16);
                for (unsigned i = 0; i < lanes; ++i) {
                    const __int128 x = lane_sext(lane_get(A, esize, i), esize);
                    const __int128 y = lane_sext(lane_get(B, esize, i), esize);
                    __int128 pr = (x * y) * 2;
                    int64_t hi = (int64_t)(pr >> esize);
                    if (hi > smax) hi = smax;
                    lane_put(R, esize, i, (uint64_t)hi & ((UINT64_C(1) << esize) - 1));
                }
                memset(&c, 0, sizeof c); load_ctx(&c); functions[slot](&c);
                if (!match(&c, Q ? 16 : 8)) { printf("FAIL sqdmulh Q=%u size=%u\n",Q,size); return 1; }
                ++checks;
            }
        }
    }

    /* A generator/runner mismatch would silently test the wrong
       function for every case after it, so check the walk consumed
       exactly the emitted table. */
    if (slot != sizeof functions / sizeof functions[0]) {
        printf("dispatch desync: walked %u of %zu\n", slot,
               sizeof functions / sizeof functions[0]);
        return 1;
    }
    printf("%d SIMD comparisons passed\n", checks);
    return 0;
}
)C";
    return 0;
}
