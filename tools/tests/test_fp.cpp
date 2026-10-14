// SPDX-License-Identifier: GPL-2.0-or-later
// Emits an executable test of the scalar floating-point instructions added
// alongside ADC/SBC: the three-source fused forms, the FRINT family, the
// integer and fixed-point conversions with every rounding mode, and FCCMP.
#include <cassert>
#include <iostream>
#include <string>
#include "core/recompiler/arm64_to_c.h"

// The generated runtime is the implementation under test for the helpers the
// emitted code calls, so lift them out of RuntimeC() rather than restating.
// Some helpers live in the header rather than the runtime source: the flag and
// condition ones were moved there so the compiler can see they touch only four
// bytes of flags, which stops it reloading the whole guest register file around
// every compare. Look in both, so moving one does not silently break the test.
static std::string RuntimeFunction(const std::string& signature) {
    const std::string runtime = std::string{suyu::recomp::RuntimeC()} + suyu::recomp::RuntimeH();
    const size_t start = runtime.find(signature);
    assert(start != std::string::npos && "function missing from RuntimeC()");
    const size_t end = runtime.find("\n}\n", start);
    assert(end != std::string::npos && "function has no closing brace");
    return runtime.substr(start, end - start + 3) + "\n";
}

static unsigned slot = 0;
static std::string dispatch;

static void emit(uint32_t insn) {
    std::string body;
    if (!suyu::recomp::Translate(insn, 0, body)) {
        std::cerr << "Translate refused " << std::hex << insn << '\n';
        std::exit(1);
    }
    if (body.find("recomp_unhandled") != std::string::npos) {
        std::cerr << "unhandled " << std::hex << insn << '\n';
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
typedef struct GuestContext {
    uint64_t x[32];
    uint8_t n,z,c,v;
    uint64_t vreg[32][2];
} GuestContext;
)C";
    std::cout << RuntimeFunction("static inline int recomp_cond(GuestContext* c,unsigned cond)");

    // rn=1, rm=2, ra=3, rd=0 throughout, so one fixed register layout serves
    // every case and the runner can set the operands once.
    for (unsigned ftype = 0; ftype < 2; ++ftype)
        for (unsigned o1 = 0; o1 < 2; ++o1)
            for (unsigned o0 = 0; o0 < 2; ++o0)
                emit(0x1F000000u | (ftype << 22) | (o1 << 21) | (2u << 16) |
                     (o0 << 15) | (3u << 10) | (1u << 5) | 0u);

    static const unsigned rint_opcodes[] = {8, 9, 10, 11, 12, 14, 15};
    for (unsigned ftype = 0; ftype < 2; ++ftype)
        for (unsigned op : rint_opcodes)
            emit(0x1E204000u | (ftype << 22) | (op << 15) | (1u << 5) | 0u);

    // FCVT{N,P,M,Z}{S,U} and FCVTA{S,U}: rmode in bits 20..19, opcode in 18..16.
    for (unsigned sf = 0; sf < 2; ++sf)
        for (unsigned ftype = 0; ftype < 2; ++ftype) {
            for (unsigned rmode = 0; rmode < 4; ++rmode)
                for (unsigned op = 0; op < 2; ++op)
                    emit(0x1E200000u | (sf << 31) | (ftype << 22) | (rmode << 19) |
                         (op << 16) | (1u << 5) | 0u);
            for (unsigned op = 4; op < 6; ++op)
                emit(0x1E200000u | (sf << 31) | (ftype << 22) | (op << 16) | (1u << 5) | 0u);
        }

    // Fixed-point conversions. fbits is 64 - scale; exercise a small, a middle
    // and the widest legal value for each operand width.
    static const unsigned fbits_list[] = {1, 8, 31};
    for (unsigned sf = 0; sf < 2; ++sf)
        for (unsigned ftype = 0; ftype < 2; ++ftype)
            for (unsigned fb : fbits_list) {
                const unsigned scale = 64 - fb;
                for (unsigned op = 2; op < 4; ++op)     // SCVTF / UCVTF
                    emit(0x1E000000u | (sf << 31) | (ftype << 22) | (op << 16) |
                         (scale << 10) | (1u << 5) | 0u);
                for (unsigned op = 0; op < 2; ++op)     // FCVTZS / FCVTZU
                    emit(0x1E000000u | (sf << 31) | (ftype << 22) | (3u << 19) |
                         (op << 16) | (scale << 10) | (1u << 5) | 0u);
            }

    // FCCMP: every condition, both nzcv extremes.
    for (unsigned ftype = 0; ftype < 2; ++ftype)
        for (unsigned cond = 0; cond < 16; ++cond)
            for (unsigned nzcv = 0; nzcv < 16; nzcv += 5)
                emit(0x1E200400u | (ftype << 22) | (2u << 16) | (cond << 12) |
                     (1u << 5) | nzcv);

    std::cout << "static void (*functions[])(GuestContext*) = {" << dispatch << "};\n";
    std::cout << R"C(
/* Reference models. Every one is written from the architectural description
   and uses long double and a different set of library calls than the emitted
   code, so a wrong opcode-to-operation mapping cannot cancel out. */
static long double ref_rint(long double x) {          /* nearest, ties to even */
    long double r = floorl(x), d = x - r;
    if (d > 0.5L) r += 1.0L;
    else if (d == 0.5L && fmodl(r, 2.0L) != 0.0L) r += 1.0L;
    return r;
}
static long double ref_round(long double x) {         /* nearest, ties away */
    return x < 0.0L ? -floorl(-x + 0.5L) : floorl(x + 0.5L);
}
static long double ref_apply(unsigned mode, long double x) {
    long double r;
    switch (mode) {
    case 0: r = ref_rint(x); break;
    case 1: r = -floorl(-x); break;   /* toward +inf */
    case 2: r = floorl(x); break;     /* toward -inf */
    case 3: r = truncl(x); break;     /* toward zero */
    default: r = ref_round(x); break; /* ties away from zero */
    }
    /* Rounding never changes the sign, so a zero result keeps the operand's
       sign - the formulas above lose that for the ties-to-even case. */
    return r == 0.0L ? copysignl(0.0L, x) : r;
}
static int checks = 0;

static uint64_t bits_of_f(float v) { uint32_t b; memcpy(&b,&v,4); return b; }
static uint64_t bits_of_d(double v) { uint64_t b; memcpy(&b,&v,8); return b; }
static void set_lane(GuestContext* c, unsigned r, int dbl, long double v) {
    c->vreg[r][0] = dbl ? bits_of_d((double)v) : bits_of_f((float)v);
    c->vreg[r][1] = 0;
}
static long double get_lane(const GuestContext* c, unsigned r, int dbl) {
    if (dbl) { double d; memcpy(&d,&c->vreg[r][0],8); return (long double)d; }
    float f; memcpy(&f,&c->vreg[r][0],4); return (long double)f;
}
static int same_fp(const GuestContext* c, unsigned r, int dbl, long double want) {
    if (dbl) {
        double got, w = (double)want;
        memcpy(&got,&c->vreg[r][0],8);
        if (got != got || w != w) return got != got && w != w;
        return bits_of_d(got) == bits_of_d(w);
    }
    float got, w = (float)want;
    memcpy(&got,&c->vreg[r][0],4);
    if (got != got || w != w) return got != got && w != w;
    return bits_of_f(got) == bits_of_f(w);
}

int main(void) {
    unsigned slot = 0;
    GuestContext c;

    /* Three-source. The operands are small multiples of a half so the product
       and the sum are both exact in every format involved; that isolates the
       operand and sign mapping, which is what this encoding gets wrong. */
    static const long double tri[][3] = {
        {2.0L, 3.0L, 5.0L}, {-2.0L, 3.0L, 5.0L}, {2.0L, -3.0L, 0.5L},
        {0.0L, 7.0L, -4.0L}, {1.5L, -2.5L, 8.0L}, {-1.5L, -2.5L, -8.0L},
    };
    for (unsigned ftype = 0; ftype < 2; ++ftype)
    for (unsigned o1 = 0; o1 < 2; ++o1)
    for (unsigned o0 = 0; o0 < 2; ++o0, ++slot)
    for (unsigned t = 0; t < sizeof tri / sizeof tri[0]; ++t) {
        memset(&c, 0, sizeof c);
        set_lane(&c, 1, ftype, tri[t][0]);
        set_lane(&c, 2, ftype, tri[t][1]);
        set_lane(&c, 3, ftype, tri[t][2]);
        long double n = get_lane(&c,1,ftype), m = get_lane(&c,2,ftype), a = get_lane(&c,3,ftype);
        long double prod = ((o0 ^ o1) ? -1.0L : 1.0L) * (n * m);
        long double acc  = (o1 ? -1.0L : 1.0L) * a;
        functions[slot](&c);
        if (!same_fp(&c, 0, ftype, prod + acc)) { printf("FAIL tri ftype=%u o1=%u o0=%u t=%u\n",ftype,o1,o0,t); return 1; }
        ++checks;
    }

    /* FRINT. Halves, negatives, an already-integral value and a magnitude
       past the point where every value is integral. */
    static const long double rints[] = {
        0.5L, 1.5L, 2.5L, -0.5L, -1.5L, -2.5L, 0.25L, -0.25L,
        3.0L, -3.0L, 0.0L, 1e9L, -1e9L,
    };
    static const unsigned rint_mode[] = {0, 1, 2, 3, 4, 0, 0}; /* N P M Z A X I */
    for (unsigned ftype = 0; ftype < 2; ++ftype)
    for (unsigned k = 0; k < 7; ++k, ++slot)
    for (unsigned t = 0; t < sizeof rints / sizeof rints[0]; ++t) {
        memset(&c, 0, sizeof c);
        set_lane(&c, 1, ftype, rints[t]);
        long double want = ref_apply(rint_mode[k], get_lane(&c,1,ftype));
        functions[slot](&c);
        if (!same_fp(&c, 0, ftype, want)) { printf("FAIL rint ftype=%u k=%u t=%u got=%La want=%La\n",ftype,k,t,get_lane(&c,0,ftype),want); return 1; }
        ++checks;
    }

    /* Conversions to integer, including the out-of-range and NaN cases that
       must saturate rather than take a bare C cast. */
    static const long double cvts[] = {
        0.0L, 0.5L, 1.5L, 2.5L, -0.5L, -1.5L, -2.5L, 3.75L, -3.75L,
        1e30L, -1e30L, 2147483647.0L, -2147483648.0L, 4294967295.0L,
        9.3e18L, -9.3e18L,
    };
    for (unsigned sf = 0; sf < 2; ++sf)
    for (unsigned ftype = 0; ftype < 2; ++ftype) {
        for (unsigned rmode = 0; rmode < 4; ++rmode)
        for (unsigned op = 0; op < 2; ++op, ++slot)
        for (unsigned t = 0; t < sizeof cvts / sizeof cvts[0]; ++t) {
            memset(&c, 0, sizeof c);
            set_lane(&c, 1, ftype, cvts[t]);
            long double a = get_lane(&c,1,ftype);
            long double r = ref_apply(rmode, a);
            uint64_t want;
            if (a != a) want = 0;
            else if (op == 0) {
                long double lo = sf ? -9223372036854775808.0L : -2147483648.0L;
                long double hi = sf ?  9223372036854775807.0L :  2147483647.0L;
                if (r <= lo) want = sf ? 0x8000000000000000ULL : 0xFFFFFFFF80000000ULL;
                else if (r >= hi) want = sf ? 0x7FFFFFFFFFFFFFFFULL : 0x7FFFFFFFULL;
                else want = (uint64_t)(int64_t)r;
            } else {
                long double hi = sf ? 18446744073709551615.0L : 4294967295.0L;
                if (r <= 0.0L) want = 0;
                else if (r >= hi) want = sf ? 0xFFFFFFFFFFFFFFFFULL : 0xFFFFFFFFULL;
                else want = (uint64_t)r;
            }
            if (!sf) want &= 0xFFFFFFFFULL;
            functions[slot](&c);
            if (c.x[0] != want) { printf("FAIL cvt sf=%u ftype=%u rmode=%u op=%u t=%u got=%llx want=%llx\n",sf,ftype,rmode,op,t,(unsigned long long)c.x[0],(unsigned long long)want); return 1; }
            ++checks;
        }
        /* FCVTAS / FCVTAU share the loop body with rmode 4. */
        for (unsigned op = 0; op < 2; ++op, ++slot)
        for (unsigned t = 0; t < sizeof cvts / sizeof cvts[0]; ++t) {
            memset(&c, 0, sizeof c);
            set_lane(&c, 1, ftype, cvts[t]);
            long double a = get_lane(&c,1,ftype);
            long double r = ref_apply(4, a);
            uint64_t want;
            if (a != a) want = 0;
            else if (op == 0) {
                long double lo = sf ? -9223372036854775808.0L : -2147483648.0L;
                long double hi = sf ?  9223372036854775807.0L :  2147483647.0L;
                if (r <= lo) want = sf ? 0x8000000000000000ULL : 0xFFFFFFFF80000000ULL;
                else if (r >= hi) want = sf ? 0x7FFFFFFFFFFFFFFFULL : 0x7FFFFFFFULL;
                else want = (uint64_t)(int64_t)r;
            } else {
                long double hi = sf ? 18446744073709551615.0L : 4294967295.0L;
                if (r <= 0.0L) want = 0;
                else if (r >= hi) want = sf ? 0xFFFFFFFFFFFFFFFFULL : 0xFFFFFFFFULL;
                else want = (uint64_t)r;
            }
            if (!sf) want &= 0xFFFFFFFFULL;
            functions[slot](&c);
            if (c.x[0] != want) { printf("FAIL cvta sf=%u ftype=%u op=%u t=%u got=%llx want=%llx\n",sf,ftype,op,t,(unsigned long long)c.x[0],(unsigned long long)want); return 1; }
            ++checks;
        }
    }

    /* Fixed point. The integer operand carries fbits fraction bits. */
    static const unsigned fbits_list[] = {1, 8, 31};
    static const uint64_t fixed_in[] = {0, 1, 256, 0xFFFFFFFFULL, 0x7FFFFFFFULL,
                                        0xFFFFFFFF80000000ULL, 0x8000000000000000ULL};
    static const long double fixed_fp[] = {0.0L, 0.5L, 1.0L, 2.0L, -1.0L, 1e12L, -1e12L};
    for (unsigned sf = 0; sf < 2; ++sf)
    for (unsigned ftype = 0; ftype < 2; ++ftype)
    for (unsigned f = 0; f < 3; ++f) {
        const unsigned fb = fbits_list[f];
        long double scale = powl(2.0L, (long double)fb);
        for (unsigned op = 2; op < 4; ++op, ++slot)
        for (unsigned t = 0; t < sizeof fixed_in / sizeof fixed_in[0]; ++t) {
            memset(&c, 0, sizeof c);
            c.x[1] = fixed_in[t];
            long double src;
            if (op == 2) src = sf ? (long double)(int64_t)c.x[1]
                                  : (long double)(int32_t)(uint32_t)c.x[1];
            else         src = sf ? (long double)c.x[1]
                                  : (long double)(uint32_t)c.x[1];
            /* Divide in the operand's own format, as the emitted code does;
               converting in long double first would round differently. */
            long double want = ftype ? (long double)((double)src / (double)scale)
                                     : (long double)((float)src / (float)scale);
            functions[slot](&c);
            if (!same_fp(&c, 0, ftype, want)) { printf("FAIL fix2fp sf=%u ftype=%u fb=%u op=%u t=%u got=%La want=%La\n",sf,ftype,fb,op,t,get_lane(&c,0,ftype),want); return 1; }
            ++checks;
        }
        for (unsigned op = 0; op < 2; ++op, ++slot)
        for (unsigned t = 0; t < sizeof fixed_fp / sizeof fixed_fp[0]; ++t) {
            memset(&c, 0, sizeof c);
            set_lane(&c, 1, ftype, fixed_fp[t]);
            long double a = get_lane(&c,1,ftype);
            /* Scale in the operand's own format, matching the emitted code. */
            long double scaled = ftype ? (long double)((double)a * (double)scale)
                                       : (long double)((float)a * (float)scale);
            long double r = truncl(scaled);
            uint64_t want;
            if (scaled != scaled) want = 0;
            else if (op == 0) {
                long double lo = sf ? -9223372036854775808.0L : -2147483648.0L;
                long double hi = sf ?  9223372036854775807.0L :  2147483647.0L;
                if (r <= lo) want = sf ? 0x8000000000000000ULL : 0xFFFFFFFF80000000ULL;
                else if (r >= hi) want = sf ? 0x7FFFFFFFFFFFFFFFULL : 0x7FFFFFFFULL;
                else want = (uint64_t)(int64_t)r;
            } else {
                long double hi = sf ? 18446744073709551615.0L : 4294967295.0L;
                if (r <= 0.0L) want = 0;
                else if (r >= hi) want = sf ? 0xFFFFFFFFFFFFFFFFULL : 0xFFFFFFFFULL;
                else want = (uint64_t)r;
            }
            if (!sf) want &= 0xFFFFFFFFULL;
            functions[slot](&c);
            if (c.x[0] != want) { printf("FAIL fp2fix sf=%u ftype=%u fb=%u op=%u t=%u got=%llx want=%llx\n",sf,ftype,fb,op,t,(unsigned long long)c.x[0],(unsigned long long)want); return 1; }
            ++checks;
        }
    }

    /* FCCMP. When the condition holds it must behave exactly as FCMP; when it
       does not, NZCV comes from the immediate and nothing else. */
    static const long double pairs[][2] = {
        {1.0L, 2.0L}, {2.0L, 1.0L}, {1.0L, 1.0L}, {-1.0L, 1.0L},
    };
    for (unsigned ftype = 0; ftype < 2; ++ftype)
    for (unsigned cond = 0; cond < 16; ++cond)
    for (unsigned nzcv = 0; nzcv < 16; nzcv += 5, ++slot)
    for (unsigned flags = 0; flags < 16; ++flags)
    for (unsigned t = 0; t < 5; ++t) {
        memset(&c, 0, sizeof c);
        c.n = (flags>>3)&1; c.z = (flags>>2)&1; c.c = (flags>>1)&1; c.v = flags&1;
        const int nan_case = (t == 4);
        if (nan_case) {
            set_lane(&c, 1, ftype, 0.0L);
            c.vreg[1][0] = ftype ? 0x7FF8000000000000ULL : 0x7FC00000ULL;
            set_lane(&c, 2, ftype, 1.0L);
        } else {
            set_lane(&c, 1, ftype, pairs[t][0]);
            set_lane(&c, 2, ftype, pairs[t][1]);
        }
        const int taken = recomp_cond(&c, cond);
        int wn, wz, wc, wv;
        if (taken) {
            if (nan_case) { wn = 0; wz = 0; wc = 1; wv = 1; }
            else {
                long double a = get_lane(&c,1,ftype), b = get_lane(&c,2,ftype);
                wn = a < b; wz = a == b; wc = a >= b; wv = 0;
            }
        } else {
            wn = (nzcv>>3)&1; wz = (nzcv>>2)&1; wc = (nzcv>>1)&1; wv = nzcv&1;
        }
        functions[slot](&c);
        if (c.n != wn || c.z != wz || c.c != wc || c.v != wv) { printf("FAIL fccmp ftype=%u cond=%u nzcv=%u flags=%u t=%u\n",ftype,cond,nzcv,flags,t); return 1; }
        ++checks;
    }

    /* A generator/runner mismatch would silently test the wrong
       function for every case after it, so check the walk consumed
       exactly the emitted table. */
    if (slot != sizeof functions / sizeof functions[0]) {
        printf("dispatch desync: walked %u of %zu\n", slot,
               sizeof functions / sizeof functions[0]);
        return 1;
    }
    printf("%d scalar FP comparisons passed\n", checks);
    return 0;
}
)C";
    return 0;
}
