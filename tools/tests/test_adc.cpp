// SPDX-License-Identifier: GPL-2.0-or-later
// Emits an executable test of every ADC/ADCS/SBC/SBCS encoding shape.
#include <cassert>
#include <iostream>
#include <string>
#include "core/recompiler/arm64_to_c.h"

// The implementation under test is emitted as C text inside RuntimeC(), so lift
// it out verbatim rather than restating it here - a copy would drift and would
// stop the test from covering what actually ships.
static std::string AddWithCarryImplementation() {
    const std::string runtime = suyu::recomp::RuntimeC();
    const std::string start_marker = "uint64_t recomp_add_with_carry(GuestContext* c";
    const size_t start = runtime.find(start_marker);
    assert(start != std::string::npos && "recomp_add_with_carry missing from RuntimeC()");
    const size_t end = runtime.find("\n}\n", start);
    assert(end != std::string::npos && "recomp_add_with_carry has no closing brace");
    return runtime.substr(start, end - start + 3);
}

int main() {
    std::cout << R"C(
#include <stdint.h>
#include <stdio.h>
typedef struct GuestContext { uint64_t x[32]; uint8_t n,z,c,v; } GuestContext;
)C";
    std::cout << AddWithCarryImplementation();
    std::cout << R"C(
/* Reference model, written from the architectural description rather than from
   the implementation: a 128-bit unsigned sum gives the carry and a 128-bit
   signed sum gives the overflow, so neither borrows the emitter's formula. */
static int64_t sext(uint64_t v, unsigned width) {
    return width == 64 ? (int64_t)v : (int64_t)(int32_t)(uint32_t)v;
}
static void reference(unsigned sf, unsigned op, uint64_t a, uint64_t b, unsigned carry,
                      uint64_t* out_r, int* out_n, int* out_z, int* out_c, int* out_v) {
    const unsigned width = sf ? 64 : 32;
    const uint64_t mask = sf ? UINT64_MAX : UINT32_MAX;
    const uint64_t aw = a & mask;
    const uint64_t bw = (op ? ~b : b) & mask;
    const __uint128_t usum = (__uint128_t)aw + (__uint128_t)bw + carry;
    const uint64_t r = (uint64_t)usum & mask;
    const __int128 ssum = (__int128)sext(aw, width) + (__int128)sext(bw, width) + carry;
    *out_r = r;
    *out_n = (int)((r >> (width - 1)) & 1);
    *out_z = r == 0;
    *out_c = (int)((usum >> width) & 1);
    *out_v = ssum != (__int128)sext(r, width);
}
)C";

    // rn/rm/rd each as a real register or as XZR; register 31 is never SP in
    // this class, so the XZR cases have to behave as reads of zero and as a
    // discarded write.
    const unsigned rn_choice[2] = {1, 31};
    const unsigned rm_choice[2] = {2, 31};
    const unsigned rd_choice[2] = {0, 31};

    unsigned index = 0;
    for (unsigned sf = 0; sf < 2; ++sf)
        for (unsigned op = 0; op < 2; ++op)
            for (unsigned S = 0; S < 2; ++S)
                for (unsigned n = 0; n < 2; ++n)
                    for (unsigned m = 0; m < 2; ++m)
                        for (unsigned d = 0; d < 2; ++d) {
                            const uint32_t insn = 0x1A000000U | (sf << 31) | (op << 30) |
                                                  (S << 29) | (rm_choice[m] << 16) |
                                                  (rn_choice[n] << 5) | rd_choice[d];
                            std::string emitted;
                            if (!suyu::recomp::Translate(insn, 0, emitted)) {
                                std::cerr << "Translate refused " << std::hex << insn << '\n';
                                return 1;
                            }
                            std::cout << "static void f" << index++ << "(GuestContext* c){"
                                      << emitted << "}\n";
                        }

    std::cout << "static void (*functions[])(GuestContext*) = {";
    for (unsigned i = 0; i < index; ++i) std::cout << "f" << i << ',';
    std::cout << R"C(};
int main(void) {
    const unsigned rn_choice[2] = {1, 31};
    const unsigned rm_choice[2] = {2, 31};
    const unsigned rd_choice[2] = {0, 31};
    const uint64_t samples[] = {
        0, 1, UINT64_MAX,
        UINT64_C(0x7fffffff), UINT64_C(0x80000000),
        UINT64_C(0x7fffffffffffffff), UINT64_C(0x8000000000000000),
        UINT64_C(0x0123456789abcdef),
    };
    const unsigned sample_count = sizeof samples / sizeof samples[0];
    unsigned index = 0, checks = 0;
    for (unsigned sf = 0; sf < 2; ++sf)
    for (unsigned op = 0; op < 2; ++op)
    for (unsigned S = 0; S < 2; ++S)
    for (unsigned n = 0; n < 2; ++n)
    for (unsigned m = 0; m < 2; ++m)
    for (unsigned d = 0; d < 2; ++d, ++index)
    for (unsigned carry = 0; carry < 2; ++carry)
    for (unsigned ia = 0; ia < sample_count; ++ia)
    for (unsigned ib = 0; ib < sample_count; ++ib) {
        GuestContext c;
        for (unsigned i = 0; i < 32; ++i) c.x[i] = 0;
        c.x[1] = samples[ia];
        c.x[2] = samples[ib];
        /* Seed the untouched flags with a pattern the reference is unlikely to
           reproduce by accident, so a missing write shows up as a mismatch and
           a spurious write shows up in the S == 0 case. */
        c.n = 1; c.z = 1; c.v = 1; c.c = (uint8_t)carry;
        const uint64_t a = rn_choice[n] == 31 ? 0 : c.x[rn_choice[n]];
        const uint64_t b = rm_choice[m] == 31 ? 0 : c.x[rm_choice[m]];
        const uint64_t before = c.x[rd_choice[d]];

        uint64_t expected_r; int expected_n, expected_z, expected_c, expected_v;
        reference(sf, op, a, b, carry, &expected_r, &expected_n, &expected_z,
                  &expected_c, &expected_v);

        functions[index](&c);

        if (rd_choice[d] == 31) {
            /* The destination is the zero register: nothing may be written. */
            if (c.x[31] != before) return 1;
        } else {
            if (c.x[rd_choice[d]] != expected_r) return 1;
        }
        if (S) {
            if (c.n != expected_n || c.z != expected_z ||
                c.c != expected_c || c.v != expected_v) return 1;
        } else {
            /* ADC and SBC leave NZCV alone. */
            if (c.n != 1 || c.z != 1 || c.v != 1 || c.c != carry) return 1;
        }
        ++checks;
    }
    printf("%u ADC/SBC comparisons passed\n", checks);
}
)C";
}
