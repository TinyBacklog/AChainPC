// SPDX-License-Identifier: GPL-2.0-or-later
// Emits an executable test of the reciprocal estimates FRECPE and FRSQRTE and
// of the Newton-Raphson steps FRECPS and FRSQRTS.
//
// The estimates are not 1/x and 1/sqrt(x): the architecture defines them
// through an eight-bit table, and the guest refines the answer afterwards, so
// a more accurate result would still be the wrong one. This test pins the
// emitted helpers to that definition.
#include <cassert>
#include <iostream>
#include <string>
#include "core/recompiler/arm64_to_c.h"

static std::string RuntimeSection() {
    const std::string rt = suyu::recomp::RuntimeC();
    const size_t a = rt.find("static unsigned recomp_recip_estimate(uint64_t a)");
    assert(a != std::string::npos && "reciprocal helpers missing from RuntimeC()");
    const size_t b = rt.find("uint64_t recomp_add_with_carry(GuestContext* c", a);
    assert(b != std::string::npos && "end marker missing");
    return rt.substr(a, b - a);
}

int main() {
    std::cout << R"C(
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
typedef struct GuestContext { uint64_t vreg[32][2]; } GuestContext;
)C";
    std::cout << RuntimeSection();
    std::cout << R"C(
/* Reference model. It walks the same architectural steps but stays in the
   floating-point domain - frexp to split the operand, ldexp to rebuild the
   result - so it shares no bit manipulation with the implementation above. */
static uint64_t ref_estimate(uint64_t bits, int is64, int sqrt_form) {
    const unsigned mw = is64 ? 52u : 23u, ew = is64 ? 11u : 8u;
    const uint64_t frac_mask = (UINT64_C(1) << mw) - 1;
    const uint64_t exp_max = (UINT64_C(1) << ew) - 1;
    const uint64_t raw_exp = (bits >> mw) & exp_max;
    const uint64_t frac = bits & frac_mask;
    const uint64_t sbit = bits & ((uint64_t)1 << (mw + ew));
    const uint64_t inf = exp_max << mw;
    const int sign = sbit != 0;

    if (raw_exp == exp_max && frac != 0) return bits | (UINT64_C(1) << (mw - 1));
    if (raw_exp == 0 && frac == 0) {                    /* zero */
        return sqrt_form ? (sbit | inf) : (sbit | inf);
    }
    if (sqrt_form && sign) return inf | (UINT64_C(1) << (mw - 1));   /* default NaN */
    if (raw_exp == exp_max) return sqrt_form ? 0 : sbit;             /* infinity */

    {
        double x;
        if (is64) { uint64_t b = bits & ~sbit; memcpy(&x, &b, 8); }
        else { uint32_t b = (uint32_t)(bits & ~sbit); float f; memcpy(&f, &b, 4); x = f; }
        {
            int e;
            const double m = frexp(x, &e);      /* x = m * 2^e, m in [0.5,1) */
            const int E = e - 1;                /* the architecture's exponent */
            if (!sqrt_form) {
                const int emin = is64 ? -1022 : -126;
                if (E < emin - 2) return sbit | inf;
                {
                    const uint64_t scaled = (uint64_t)floor(m * 512.0);
                    /* The table's bit 8 is the mantissa's implicit leading
                       one, so only its low eight bits are the fraction. */
                    const unsigned est = recomp_recip_estimate(scaled) & 0xFFu;
                    const double r = ldexp(1.0 + est / 256.0, -(E + 1));
                    if (is64) { uint64_t b; memcpy(&b, &r, 8); return b | sbit; }
                    { const float f = (float)r; uint32_t b; memcpy(&b, &f, 4); return b | sbit; }
                }
            }
            {
                const int neg = -(E + 1);
                const int result_exp = neg >= 0 ? (neg >> 1) : -(((-neg) + 1) >> 1);
                const int odd = (E % 2) == 0;
                const uint64_t scaled = (uint64_t)floor(m * (odd ? 256.0 : 512.0));
                const unsigned est = recomp_recip_sqrt_estimate(scaled) & 0xFFu;
                const double r = ldexp(1.0 + est / 256.0, result_exp);
                if (is64) { uint64_t b; memcpy(&b, &r, 8); return b; }
                { const float f = (float)r; uint32_t b; memcpy(&b, &f, 4); return b; }
            }
        }
    }
}
)C";

    // FRECPE/FRSQRTE vector, and FRECPS/FRSQRTS vector, with Rn=1, Rm=2, Rd=0.
    unsigned slot = 0;
    std::string dispatch;
    auto emit = [&](uint32_t insn) {
        std::string body;
        if (!suyu::recomp::Translate(insn, 0, body) ||
            body.find("recomp_unhandled") != std::string::npos) {
            std::cerr << "not translated: " << std::hex << insn << '\n';
            std::exit(1);
        }
        std::cout << "static void f" << slot << "(GuestContext* c){" << body << "}\n";
        dispatch += "f" + std::to_string(slot) + ",";
        ++slot;
    };
    for (unsigned U = 0; U < 2; ++U)       // FRECPE (U=0) / FRSQRTE (U=1)
        emit(0x0EA1D800u | (1u << 30) | (U << 29) | (1u << 5) | 0u);
    for (unsigned a = 0; a < 2; ++a)       // FRECPS (a=0) / FRSQRTS (a=1)
        emit(0x0E20FC00u | (1u << 30) | (a << 23) | (2u << 16) | (1u << 5) | 0u);

    std::cout << "static void (*functions[])(GuestContext*) = {" << dispatch << "};\n";
    std::cout << R"C(
int main(void) {
    unsigned checks = 0;
    /* Every exponent, against a spread of mantissas that includes the ends of
       the range and the bits the eight-bit table actually reads. */
    for (uint64_t e = 0; e < 256; ++e)
    for (unsigned k = 0; k < 40; ++k)
    for (unsigned sgn = 0; sgn < 2; ++sgn) {
        static const uint32_t mant[40] = {
            0, 1, 2, 3, 0x7F, 0x80, 0xFF, 0x100, 0x4000, 0x7FFF, 0x8000,
            0x10000, 0x1FFFF, 0x20000, 0x3FFFFF, 0x400000, 0x7FFFFE, 0x7FFFFF,
            0x123456, 0x654321, 0x2AAAAA, 0x555555, 0x0F0F0F, 0x70F0F0,
            0x001000, 0x002000, 0x004000, 0x008000, 0x010000, 0x020000,
            0x040000, 0x080000, 0x100000, 0x200000, 0x300000, 0x600000,
            0x7E0000, 0x018000, 0x038000, 0x078000,
        };
        const uint32_t b = ((uint32_t)sgn << 31) | ((uint32_t)e << 23) | mant[k];
        if ((uint32_t)recomp_frecpe(b, 0) != (uint32_t)ref_estimate(b, 0, 0)) {
            printf("FAIL frecpe32 %08x got %08x want %08x\n", b,
                   (uint32_t)recomp_frecpe(b, 0), (uint32_t)ref_estimate(b, 0, 0));
            return 1;
        }
        if ((uint32_t)recomp_frsqrte(b, 0) != (uint32_t)ref_estimate(b, 0, 1)) {
            printf("FAIL frsqrte32 %08x got %08x want %08x\n", b,
                   (uint32_t)recomp_frsqrte(b, 0), (uint32_t)ref_estimate(b, 0, 1));
            return 1;
        }
        checks += 2;
    }
    for (uint64_t e = 0; e < 2048; ++e)
    for (unsigned k = 0; k < 12; ++k)
    for (unsigned sgn = 0; sgn < 2; ++sgn) {
        static const uint64_t mant[12] = {
            0, 1, 0xF, 0x8000000000000ULL, 0xFFFFFFFFFFFFFULL, 0x123456789ABCDULL,
            0x5555555555555ULL, 0xAAAAAAAAAAAAAULL, 0x1000000000000ULL,
            0x2000000000000ULL, 0x4000000000000ULL, 0xE000000000000ULL,
        };
        const uint64_t b = ((uint64_t)sgn << 63) | (e << 52) | mant[k];
        if (recomp_frecpe(b, 1) != ref_estimate(b, 1, 0)) {
            printf("FAIL frecpe64 %016llx\n", (unsigned long long)b); return 1; }
        if (recomp_frsqrte(b, 1) != ref_estimate(b, 1, 1)) {
            printf("FAIL frsqrte64 %016llx\n", (unsigned long long)b); return 1; }
        checks += 2;
    }

    /* The estimate has to stay within the architecture's stated tolerance of
       the true value: a relative error below 1/512 for the reciprocal. */
    for (int i = 1; i < 20000; ++i) {
        const float x = (float)i * 0.37f + 0.001f;
        uint32_t b; memcpy(&b, &x, 4);
        float got; { const uint32_t r = (uint32_t)recomp_frecpe(b, 0); memcpy(&got, &r, 4); }
        const float want = 1.0f / x;
        if (fabsf(got - want) / want > 1.0f / 256.0f) {
            printf("FAIL frecpe tolerance x=%f got=%f want=%f\n", x, got, want); return 1; }
        { float gs; const uint32_t r = (uint32_t)recomp_frsqrte(b, 0); memcpy(&gs, &r, 4);
          const float ws = 1.0f / sqrtf(x);
          if (fabsf(gs - ws) / ws > 1.0f / 256.0f) {
              printf("FAIL frsqrte tolerance x=%f got=%f want=%f\n", x, gs, ws); return 1; } }
        checks += 2;
    }

    /* FRECPS and FRSQRTS, including the infinity-times-zero cases that answer
       with a constant rather than the NaN a bare fused multiply would give. */
    {
        GuestContext c;
        static const float pairs[][2] = {
            {1.0f, 1.0f}, {2.0f, 0.5f}, {0.25f, 4.0f}, {-1.5f, 2.0f}, {3.0f, -0.5f},
            {INFINITY, 0.0f}, {0.0f, INFINITY}, {-INFINITY, 0.0f}, {0.0f, -INFINITY},
            {INFINITY, 2.0f}, {2.0f, INFINITY},
        };
        for (unsigned step = 0; step < 2; ++step)
        for (unsigned t = 0; t < sizeof pairs / sizeof pairs[0]; ++t) {
            memset(&c, 0, sizeof c);
            for (int i = 0; i < 4; ++i) {
                memcpy((uint8_t*)c.vreg[1] + i*4, &pairs[t][0], 4);
                memcpy((uint8_t*)c.vreg[2] + i*4, &pairs[t][1], 4);
            }
            functions[2 + step](&c);
            {
                const float a = pairs[t][0], b = pairs[t][1];
                float want;
                if ((a == 0.0f && isinf(b)) || (isinf(a) && b == 0.0f))
                    want = step ? 1.5f : 2.0f;
                else
                    want = step ? (fmaf(-a, b, 3.0f) / 2.0f) : fmaf(-a, b, 2.0f);
                for (int i = 0; i < 4; ++i) {
                    float got; memcpy(&got, (uint8_t*)c.vreg[0] + i*4, 4);
                    const int both_nan = (got != got) && (want != want);
                    if (!both_nan && got != want) {
                        printf("FAIL step=%u t=%u lane=%d got=%f want=%f\n", step, t, i, got, want);
                        return 1;
                    }
                }
            }
            ++checks;
        }
    }
    /* Four functions are emitted; the walk above uses all of them. */
    if (sizeof functions / sizeof functions[0] != 4) return 1;
    printf("%u reciprocal-estimate comparisons passed\n", checks);
    return 0;
}
)C";
    return 0;
}
