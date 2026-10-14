// SPDX-License-Identifier: GPL-2.0-or-later
// Emits an executable test of the saturating and widening SIMD forms added
// after the crypto batch: SQADD/SQSUB, PMULL, the narrowing saturating shifts
// (vector and scalar), FCVTL/FCVTN, the vector FRINT family and the widening
// multiply-accumulate by indexed element.
//
// Every reference model here is written from the architectural definition and
// deliberately differently from the emitted code: the shifts round in 128-bit
// unbounded precision instead of shifting first, the polynomial multiply walks
// the bits of the other operand, and the rounding modes are built from integer
// truncation instead of calling libm.
#include <iostream>
#include <string>
#include "core/recompiler/arm64_to_c.h"

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
)C";

    // ── SQADD/UQADD/SQSUB/UQSUB: Vd=0, Vn=1, Vm=2 ──
    std::string sat_table = "static const struct { unsigned op,u,size,q; } sat_cases[] = {";
    for (unsigned op : {0x01u, 0x05u})
        for (unsigned u = 0; u < 2; ++u)
            for (unsigned size = 0; size < 4; ++size)
                for (unsigned q = 0; q < 2; ++q) {
                    if (size == 3 && !q) continue;   // uma faixa de 64 nao existe
                    emit(0x0E200400u | (q << 30) | (u << 29) | (size << 22) | (2u << 16) |
                         (op << 11) | (1u << 5) | 0u);
                    sat_table += "{" + std::to_string(op) + "," + std::to_string(u) + "," +
                                 std::to_string(size) + "," + std::to_string(q) + "},";
                }
    const unsigned sat_first = 0, sat_count = slot;
    sat_table += "};\n";

    // ── PMULL/PMULL2 ──
    std::string pm_table = "static const struct { unsigned size,q; } pm_cases[] = {";
    const unsigned pm_first = slot;
    for (unsigned size : {0u, 3u})
        for (unsigned q = 0; q < 2; ++q) {
            emit(0x0E20E000u | (q << 30) | (size << 22) | (2u << 16) | (1u << 5) | 0u);
            pm_table += "{" + std::to_string(size) + "," + std::to_string(q) + "},";
        }
    const unsigned pm_count = slot - pm_first;
    pm_table += "};\n";

    // ── Deslocamentos que estreitam com saturacao, forma vetorial ──
    // esize 8, 16 e 32; shift de 1 ate esize.
    std::string vn_table = "static const struct { unsigned op,u,esize,shift,q; } vn_cases[] = {";
    const unsigned vn_first = slot;
    for (unsigned op : {0x10u, 0x11u, 0x12u, 0x13u})
        for (unsigned u = 0; u < 2; ++u) {
            if ((op == 0x10 || op == 0x11) && !u) continue;   // SQSHRUN existe so com U=1
            for (unsigned esize : {8u, 16u, 32u})
                for (unsigned shift : {1u, 2u, esize / 2, esize - 1, esize})
                    for (unsigned q = 0; q < 2; ++q) {
                        const unsigned imm = 2 * esize - shift;
                        emit(0x0F000400u | (q << 30) | (u << 29) | ((imm >> 3) << 19) |
                             ((imm & 7) << 16) | (op << 11) | (1u << 5) | 0u);
                        vn_table += "{" + std::to_string(op) + "," + std::to_string(u) + "," +
                                    std::to_string(esize) + "," + std::to_string(shift) + "," +
                                    std::to_string(q) + "},";
                    }
        }
    const unsigned vn_count = slot - vn_first;
    vn_table += "};\n";

    // ── As mesmas, forma escalar ──
    std::string sn_table = "static const struct { unsigned op,u,esize,shift; } sn_cases[] = {";
    const unsigned sn_first = slot;
    for (unsigned op : {0x10u, 0x11u, 0x12u, 0x13u})
        for (unsigned u = 0; u < 2; ++u) {
            if ((op == 0x10 || op == 0x11) && !u) continue;
            for (unsigned esize : {8u, 16u, 32u})
                for (unsigned shift : {1u, 2u, esize / 2, esize - 1, esize}) {
                    const unsigned imm = 2 * esize - shift;
                    emit(0x5F000400u | (u << 29) | ((imm >> 3) << 19) | ((imm & 7) << 16) |
                         (op << 11) | (1u << 5) | 0u);
                    sn_table += "{" + std::to_string(op) + "," + std::to_string(u) + "," +
                                std::to_string(esize) + "," + std::to_string(shift) + "},";
                }
        }
    const unsigned sn_count = slot - sn_first;
    sn_table += "};\n";

    // ── FCVTL/FCVTL2 e FCVTN/FCVTN2 ──
    const unsigned cv_first = slot;
    for (unsigned opcode : {0x17u, 0x16u})
        for (unsigned q = 0; q < 2; ++q)
            emit(0x0E200800u | (q << 30) | (1u << 22) | (opcode << 12) | (1u << 5) | 0u);
    const unsigned cv_count = slot - cv_first;

    // ── FRINT* vetorial ──
    // opcode, U e bit 23 escolhem o modo; a tabela guarda o modo esperado.
    std::string fr_table = "static const struct { unsigned mode,dbl,q; } fr_cases[] = {";
    const unsigned fr_first = slot;
    struct FrCase { unsigned opcode, u, a23, mode; };
    // mode: 0 nearest-even, 1 +inf, 2 -inf, 3 zero, 4 half-away
    const FrCase kFr[] = {{0x18, 0, 0, 0}, {0x18, 0, 1, 1}, {0x19, 0, 0, 2},
                          {0x19, 0, 1, 3}, {0x18, 1, 0, 4}, {0x19, 1, 0, 0},
                          {0x19, 1, 1, 0}};
    for (const FrCase& f : kFr)
        for (unsigned dbl = 0; dbl < 2; ++dbl)
            for (unsigned q = 0; q < 2; ++q) {
                if (dbl && !q) continue;            // 1 faixa de double nao existe
                emit(0x0E200800u | (q << 30) | (f.u << 29) | (f.a23 << 23) | (dbl << 22) |
                     (f.opcode << 12) | (1u << 5) | 0u);
                fr_table += "{" + std::to_string(f.mode) + "," + std::to_string(dbl) + "," +
                            std::to_string(q) + "},";
            }
    const unsigned fr_count = slot - fr_first;
    fr_table += "};\n";

    // ── SMLAL/SMLSL/SMULL por elemento. Vd=0, Vn=1, elemento de Vm=2 ──
    std::string we_table = "static const struct { unsigned op,u,esize,index,q; } we_cases[] = {";
    const unsigned we_first = slot;
    for (unsigned op : {0x2u, 0x6u, 0xAu})
        for (unsigned u = 0; u < 2; ++u)
            for (unsigned size : {1u, 2u}) {
                const unsigned esize = 8u << size;
                const unsigned lanes = (size == 1) ? 8u : 4u;
                for (unsigned index : {0u, lanes - 1u})
                    for (unsigned q = 0; q < 2; ++q) {
                        // Rm tem quatro bits com elementos de 16; M entra no
                        // indice. Com 32 bits, M alarga Rm em vez disso.
                        unsigned H, L, M, rm4;
                        if (size == 1) { H = (index >> 2) & 1; L = (index >> 1) & 1; M = index & 1; rm4 = 2; }
                        else           { H = (index >> 1) & 1; L = index & 1; M = 0; rm4 = 2; }
                        emit(0x0F000000u | (q << 30) | (u << 29) | (size << 22) | (L << 21) |
                             (M << 20) | (rm4 << 16) | (op << 12) | (H << 11) | (1u << 5) | 0u);
                        we_table += "{" + std::to_string(op) + "," + std::to_string(u) + "," +
                                    std::to_string(esize) + "," + std::to_string(index) + "," +
                                    std::to_string(q) + "},";
                    }
            }
    const unsigned we_count = slot - we_first;
    we_table += "};\n";

    std::cout << "static void (*functions[])(GuestContext*) = {" << dispatch << "};\n";
    std::cout << sat_table << pm_table << vn_table << sn_table << fr_table << we_table;
    auto def = [](const char* n, unsigned v) {
        std::cout << "#define " << n << " " << v << "\n";
    };
    def("SAT_FIRST", sat_first); def("SAT_COUNT", sat_count);
    def("PM_FIRST", pm_first);   def("PM_COUNT", pm_count);
    def("VN_FIRST", vn_first);   def("VN_COUNT", vn_count);
    def("SN_FIRST", sn_first);   def("SN_COUNT", sn_count);
    def("CV_FIRST", cv_first);   def("CV_COUNT", cv_count);
    def("FR_FIRST", fr_first);   def("FR_COUNT", fr_count);
    def("WE_FIRST", we_first);   def("WE_COUNT", we_count);
    def("SLOT_TOTAL", slot);

    std::cout << R"C(
static int checks = 0;
typedef __int128 i128;
typedef unsigned __int128 u128;

static void run(int idx, uint64_t d[2], const uint64_t n[2], const uint64_t m[2]) {
    GuestContext c;
    memset(&c, 0, sizeof c);
    c.vreg[0][0] = d[0]; c.vreg[0][1] = d[1];
    c.vreg[1][0] = n[0]; c.vreg[1][1] = n[1];
    c.vreg[2][0] = m[0]; c.vreg[2][1] = m[1];
    functions[idx](&c);
    d[0] = c.vreg[0][0]; d[1] = c.vreg[0][1];
}

/* Amostras que batem nos extremos de cada largura, alem de valores comuns. */
static const uint64_t kSamples[] = {
    0x0000000000000000ULL, 0x0000000000000001ULL, 0xFFFFFFFFFFFFFFFFULL,
    0x7F7F7F7F7F7F7F7FULL, 0x8080808080808080ULL, 0x7FFF7FFF7FFF7FFFULL,
    0x8000800080008000ULL, 0x7FFFFFFF7FFFFFFFULL, 0x8000000080000000ULL,
    0x7FFFFFFFFFFFFFFFULL, 0x8000000000000000ULL, 0x0123456789ABCDEFULL,
    0xFEDCBA9876543210ULL, 0x5555555555555555ULL,
};
#define NSAMP (sizeof kSamples / sizeof kSamples[0])

static uint64_t lane_get(const uint64_t v[2], unsigned esize, unsigned i) {
    uint8_t b[16];
    uint64_t r = 0;
    memcpy(b, v, 8); memcpy(b + 8, v + 1, 8);
    memcpy(&r, b + i * (esize / 8), esize / 8);
    return r;
}
static i128 sext(uint64_t v, unsigned bits) {
    if (bits == 64) return (i128)(int64_t)v;
    { const uint64_t s = 1ULL << (bits - 1);
      const uint64_t m = (bits == 64) ? ~0ULL : ((1ULL << bits) - 1);
      v &= m;
      return (i128)(int64_t)((v ^ s) - s); }
}
static uint64_t trunc_to(u128 v, unsigned bits) {
    return bits == 64 ? (uint64_t)v : (uint64_t)(v & (((u128)1 << bits) - 1));
}

/* ── 1. SQADD / UQADD / SQSUB / UQSUB ── */
static int test_sat(void) {
    unsigned k, a, b;
    for (k = 0; k < SAT_COUNT; ++k) {
        const unsigned op = sat_cases[k].op, u = sat_cases[k].u;
        const unsigned esize = 8u << sat_cases[k].size, q = sat_cases[k].q;
        const unsigned lanes = (q ? 16u : 8u) / (esize / 8);
        for (a = 0; a < NSAMP; ++a) for (b = 0; b < NSAMP; ++b) {
            uint64_t d[2] = {0xDEADBEEFDEADBEEFULL, 0xCAFEBABECAFEBABEULL};
            const uint64_t n[2] = {kSamples[a], kSamples[(a + 5) % NSAMP]};
            const uint64_t m[2] = {kSamples[b], kSamples[(b + 3) % NSAMP]};
            unsigned i;
            run((int)(SAT_FIRST + k), d, n, m);
            for (i = 0; i < lanes; ++i) {
                const uint64_t x = lane_get(n, esize, i), y = lane_get(m, esize, i);
                uint64_t want;
                if (u) {
                    const u128 lo = 0, hi = esize == 64 ? (u128)~0ULL
                                                        : (((u128)1 << esize) - 1);
                    const u128 r = op == 1 ? (u128)x + (u128)y
                                           : ((u128)x >= (u128)y ? (u128)x - (u128)y : lo);
                    want = trunc_to(r > hi ? hi : r, esize);
                } else {
                    const i128 sx = sext(x, esize), sy = sext(y, esize);
                    const i128 hi = ((i128)1 << (esize - 1)) - 1;
                    const i128 lo = -((i128)1 << (esize - 1));
                    i128 r = op == 1 ? sx + sy : sx - sy;
                    if (r > hi) r = hi;
                    if (r < lo) r = lo;
                    want = trunc_to((u128)r, esize);
                }
                if (lane_get(d, esize, i) != want) {
                    printf("FAIL sat k=%u lane=%u: got %llx want %llx\n", k, i,
                           (unsigned long long)lane_get(d, esize, i),
                           (unsigned long long)want);
                    return 1;
                }
                ++checks;
            }
            if (!q && d[1] != 0) { printf("FAIL sat k=%u: upper half not cleared\n", k); return 1; }
        }
    }
    return 0;
}

/* ── 2. PMULL / PMULL2 ──
   Referencia independente: percorre os bits de _x em vez dos de _y. */
static u128 clmul_ref(uint64_t x, uint64_t y) {
    u128 r = 0;
    int i;
    for (i = 0; i < 64; ++i)
        if ((x >> i) & 1) r ^= (u128)y << i;
    return r;
}
static int test_pmull(void) {
    unsigned k, a, b;
    for (k = 0; k < PM_COUNT; ++k) {
        const unsigned size = pm_cases[k].size, q = pm_cases[k].q;
        for (a = 0; a < NSAMP; ++a) for (b = 0; b < NSAMP; ++b) {
            uint64_t d[2] = {0x1111111111111111ULL, 0x2222222222222222ULL};
            const uint64_t n[2] = {kSamples[a], kSamples[(a + 7) % NSAMP]};
            const uint64_t m[2] = {kSamples[b], kSamples[(b + 11) % NSAMP]};
            run((int)(PM_FIRST + k), d, n, m);
            if (size == 3) {
                const u128 want = clmul_ref(n[q], m[q]);
                if (d[0] != (uint64_t)want || d[1] != (uint64_t)(want >> 64)) {
                    printf("FAIL pmull.1q k=%u\n", k);
                    return 1;
                }
                ++checks;
            } else {
                unsigned i;
                const uint64_t nx = n[q], mx = m[q];
                for (i = 0; i < 8; ++i) {
                    const uint8_t xa = (uint8_t)(nx >> (8 * i)), xb = (uint8_t)(mx >> (8 * i));
                    uint16_t w = 0;
                    int j;
                    for (j = 0; j < 8; ++j) if ((xa >> j) & 1) w ^= (uint16_t)((uint16_t)xb << j);
                    if ((uint16_t)lane_get(d, 16, i) != w) {
                        printf("FAIL pmull.8h k=%u lane=%u\n", k, i);
                        return 1;
                    }
                    ++checks;
                }
            }
        }
    }
    return 0;
}

/* ── 3 e 4. Deslocamentos que estreitam, com saturacao ──
   A referencia arredonda somando meia unidade em 128 bits antes do
   deslocamento, que e a definicao arquitetural em precisao ilimitada; o
   codigo gerado desloca primeiro e soma o bit que saiu. */
static uint64_t narrow_ref(uint64_t src, unsigned esize, unsigned shift,
                           unsigned op, unsigned u) {
    const unsigned wide = esize * 2;
    const int round = (op == 0x11 || op == 0x13);
    const int unsigned_result = (op == 0x10 || op == 0x11);
    const int src_signed = !u || unsigned_result;
    if (src_signed) {
        i128 v = sext(src, wide);
        if (round) v += (i128)1 << (shift - 1);
        v >>= shift;
        if (unsigned_result) {
            const i128 hi = esize == 64 ? (i128)(u128)~0ULL : (((i128)1 << esize) - 1);
            if (v < 0) v = 0;
            if (v > hi) v = hi;
        } else {
            const i128 hi = ((i128)1 << (esize - 1)) - 1;
            const i128 lo = -((i128)1 << (esize - 1));
            if (v > hi) v = hi;
            if (v < lo) v = lo;
        }
        return trunc_to((u128)v, esize);
    } else {
        u128 v = (u128)(wide == 64 ? src : (src & (((u128)1 << wide) - 1)));
        if (round) v += (u128)1 << (shift - 1);
        v >>= shift;
        { const u128 hi = ((u128)1 << esize) - 1;
          if (v > hi) v = hi; }
        return trunc_to(v, esize);
    }
}
static int test_narrow_vec(void) {
    unsigned k, a;
    for (k = 0; k < VN_COUNT; ++k) {
        const unsigned op = vn_cases[k].op, u = vn_cases[k].u;
        const unsigned esize = vn_cases[k].esize, shift = vn_cases[k].shift;
        const unsigned q = vn_cases[k].q, lanes = 8u / (esize / 8);
        for (a = 0; a < NSAMP; ++a) {
            uint64_t d[2] = {0x3333333333333333ULL, 0x4444444444444444ULL};
            const uint64_t n[2] = {kSamples[a], kSamples[(a + 9) % NSAMP]};
            const uint64_t m[2] = {0, 0};
            const uint64_t keep = d[0];
            unsigned i;
            run((int)(VN_FIRST + k), d, n, m);
            for (i = 0; i < lanes; ++i) {
                const uint64_t src = lane_get(n, esize * 2, i);
                const uint64_t want = narrow_ref(src, esize, shift, op, u);
                const uint64_t got = lane_get(d, esize, q ? lanes + i : i);
                if (got != want) {
                    printf("FAIL narrow-vec k=%u lane=%u: got %llx want %llx\n", k, i,
                           (unsigned long long)got, (unsigned long long)want);
                    return 1;
                }
                ++checks;
            }
            if (q && d[0] != keep) {
                printf("FAIL narrow-vec k=%u: low half not preserved\n", k);
                return 1;
            }
            if (!q && d[1] != 0) {
                printf("FAIL narrow-vec k=%u: upper half not cleared\n", k);
                return 1;
            }
        }
    }
    return 0;
}
static int test_narrow_scl(void) {
    unsigned k, a;
    for (k = 0; k < SN_COUNT; ++k) {
        const unsigned op = sn_cases[k].op, u = sn_cases[k].u;
        const unsigned esize = sn_cases[k].esize, shift = sn_cases[k].shift;
        for (a = 0; a < NSAMP; ++a) {
            uint64_t d[2] = {0x5555AAAA5555AAAAULL, 0x9999999999999999ULL};
            const uint64_t n[2] = {kSamples[a], kSamples[(a + 4) % NSAMP]};
            const uint64_t m[2] = {0, 0};
            const uint64_t src = lane_get(n, esize * 2, 0);
            const uint64_t want = narrow_ref(src, esize, shift, op, u);
            run((int)(SN_FIRST + k), d, n, m);
            if (lane_get(d, esize, 0) != want) {
                printf("FAIL narrow-scl k=%u: got %llx want %llx\n", k,
                       (unsigned long long)lane_get(d, esize, 0), (unsigned long long)want);
                return 1;
            }
            /* Uma forma escalar zera tudo acima do elemento. */
            if (d[1] != 0 || (esize < 64 && (d[0] >> esize) != 0)) {
                printf("FAIL narrow-scl k=%u: register not zero-extended\n", k);
                return 1;
            }
            ++checks;
        }
    }
    return 0;
}

/* ── 5. FCVTL / FCVTN ── */
static int test_cvt(void) {
    static const double vals[] = {0.0, -0.0, 1.0, -1.5, 3.14159265358979, 1e30, -1e30,
                                  1.0 / 3.0, 65504.0, -1e-40};
    unsigned k, a;
    for (k = 0; k < CV_COUNT; ++k) {
        const int is_l = (k < 2);          /* os dois primeiros sao FCVTL */
        const unsigned q = k & 1;
        for (a = 0; a + 1 < sizeof vals / sizeof vals[0]; ++a) {
            uint64_t d[2] = {0x7777777777777777ULL, 0x8888888888888888ULL};
            uint64_t n[2] = {0, 0};
            const uint64_t m[2] = {0, 0};
            const uint64_t keep = d[0];
            if (is_l) {
                const float f[2] = {(float)vals[a], (float)vals[a + 1]};
                memcpy((uint8_t*)n + (q ? 8 : 0), f, 8);
                run((int)(CV_FIRST + k), d, n, m);
                { double got[2]; const double want[2] = {(double)f[0], (double)f[1]};
                  memcpy(got, d, 16);
                  if (memcmp(got, want, 16)) { printf("FAIL fcvtl k=%u a=%u\n", k, a); return 1; } }
            } else {
                const double g[2] = {vals[a], vals[a + 1]};
                memcpy(n, g, 16);
                run((int)(CV_FIRST + k), d, n, m);
                { float got[2]; const float want[2] = {(float)g[0], (float)g[1]};
                  memcpy(got, (const uint8_t*)d + (q ? 8 : 0), 8);
                  if (memcmp(got, want, 8)) { printf("FAIL fcvtn k=%u a=%u\n", k, a); return 1; }
                  if (q && d[0] != keep) { printf("FAIL fcvtn2 k=%u: low half lost\n", k); return 1; }
                  if (!q && d[1] != 0) { printf("FAIL fcvtn k=%u: upper half not cleared\n", k); return 1; } }
            }
            ++checks;
        }
    }
    return 0;
}

/* ── 6. FRINT* vetorial ──
   Referencia construida da truncatura inteira, sem chamar libm. */
static double rint_ref(double x, unsigned mode) {
    const double t = (double)(int64_t)x;            /* |x| < 2^62 nas amostras */
    double f = t, r;
    if (x < 0 && x != t) f = t - 1.0;               /* floor */
    switch (mode) {
    case 3: return t;                               /* toward zero */
    case 2: return f;                               /* toward -inf */
    case 1: return (x == f) ? f : f + 1.0;          /* toward +inf */
    case 4:                                         /* nearest, half away */
        r = x - t;
        if (r >= 0.5) return t + 1.0;
        if (r <= -0.5) return t - 1.0;
        return t;
    default: {                                      /* nearest, half to even */
        const double d = x - f;
        if (d > 0.5) return f + 1.0;
        if (d < 0.5) return f;
        return ((int64_t)f % 2 == 0) ? f : f + 1.0;
    }
    }
}
static int test_frint(void) {
    static const double vals[] = {0.0, 0.5, 1.5, 2.5, -0.5, -1.5, -2.5, 0.49, -0.49,
                                  0.51, -0.51, 3.0, -3.0, 123.456, -123.456, 7.0};
    unsigned k, a;
    for (k = 0; k < FR_COUNT; ++k) {
        const unsigned mode = fr_cases[k].mode, dbl = fr_cases[k].dbl, q = fr_cases[k].q;
        const unsigned lanes = (q ? 16u : 8u) / (dbl ? 8u : 4u);
        for (a = 0; a + lanes <= sizeof vals / sizeof vals[0]; ++a) {
            uint64_t d[2] = {0x1234567812345678ULL, 0x8765432187654321ULL};
            uint64_t n[2] = {0, 0};
            const uint64_t m[2] = {0, 0};
            unsigned i;
            if (dbl) { double g[2]; for (i = 0; i < lanes; ++i) g[i] = vals[a + i];
                       memcpy(n, g, lanes * 8); }
            else     { float g[4]; for (i = 0; i < lanes; ++i) g[i] = (float)vals[a + i];
                       memcpy(n, g, lanes * 4); }
            run((int)(FR_FIRST + k), d, n, m);
            for (i = 0; i < lanes; ++i) {
                const double want = rint_ref(vals[a + i], mode);
                if (dbl) { double got; memcpy(&got, (const uint8_t*)d + i * 8, 8);
                           if (got != want) { printf("FAIL frint k=%u i=%u mode=%u: %f vs %f\n",
                                                     k, i, mode, got, want); return 1; } }
                else     { float got; memcpy(&got, (const uint8_t*)d + i * 4, 4);
                           if (got != (float)want) { printf("FAIL frint k=%u i=%u mode=%u: %f vs %f\n",
                                                            k, i, mode, (double)got, want); return 1; } }
                ++checks;
            }
            if (!q && d[1] != 0) { printf("FAIL frint k=%u: upper half not cleared\n", k); return 1; }
        }
    }
    return 0;
}

/* ── 7. SMLAL / SMLSL / SMULL por elemento ── */
static int test_widen_elem(void) {
    unsigned k, a, b;
    for (k = 0; k < WE_COUNT; ++k) {
        const unsigned op = we_cases[k].op, u = we_cases[k].u;
        const unsigned esize = we_cases[k].esize, index = we_cases[k].index;
        const unsigned q = we_cases[k].q, lanes = 8u / (esize / 8);
        for (a = 0; a < NSAMP; ++a) for (b = 0; b < NSAMP; ++b) {
            uint64_t d[2] = {kSamples[(a + b) % NSAMP], kSamples[(a + 2 * b) % NSAMP]};
            const uint64_t n[2] = {kSamples[a], kSamples[(a + 6) % NSAMP]};
            const uint64_t m[2] = {kSamples[b], kSamples[(b + 8) % NSAMP]};
            uint64_t acc[2];
            unsigned i;
            acc[0] = d[0]; acc[1] = d[1];
            run((int)(WE_FIRST + k), d, n, m);
            for (i = 0; i < lanes; ++i) {
                /* Q escolhe a metade do operando estreito. */
                const uint64_t xa = lane_get(n, esize, q ? lanes + i : i);
                const uint64_t xm = lane_get(m, esize, index);
                u128 prod;
                uint64_t want;
                if (u) prod = (u128)xa * (u128)xm;
                else   prod = (u128)(i128)(sext(xa, esize) * sext(xm, esize));
                if (op == 0xA) want = trunc_to(prod, esize * 2);
                else {
                    const u128 old = (u128)lane_get(acc, esize * 2, i);
                    want = trunc_to(op == 0x6 ? old - prod : old + prod, esize * 2);
                }
                if (lane_get(d, esize * 2, i) != want) {
                    printf("FAIL widen-elem k=%u lane=%u: got %llx want %llx\n", k, i,
                           (unsigned long long)lane_get(d, esize * 2, i),
                           (unsigned long long)want);
                    return 1;
                }
                ++checks;
            }
        }
    }
    return 0;
}

int main(void) {
    if (sizeof functions / sizeof functions[0] != SLOT_TOTAL) {
        printf("FAIL: dispatch table out of step with the generator\n");
        return 1;
    }
    if (test_sat()) return 1;
    if (test_pmull()) return 1;
    if (test_narrow_vec()) return 1;
    if (test_narrow_scl()) return 1;
    if (test_cvt()) return 1;
    if (test_frint()) return 1;
    if (test_widen_elem()) return 1;
    printf("%d saturating/widening SIMD comparisons passed\n", checks);
    return 0;
}
)C";
    return 0;
}
