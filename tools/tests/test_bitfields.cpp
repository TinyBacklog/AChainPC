// SPDX-License-Identifier: GPL-2.0-or-later
// Emits an executable test of every legal immediate pair for SBFM/BFM/UBFM.
#include <iostream>
#include "core/recompiler/arm64_to_c.h"
int main() {
    std::cout << R"C(
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef struct { uint64_t x[32]; } Context;
static uint64_t ones(unsigned n) { return n == 64 ? UINT64_MAX : (UINT64_C(1) << n) - 1; }
static uint64_t ror(uint64_t x, unsigned r, unsigned width) {
    x &= ones(width);
    return r ? ((x >> r) | (x << (width-r))) & ones(width) : x;
}
/* Architectural rotate/write-mask/top-mask formulation, independent of the
   emitter's extract/insert implementation. */
static uint64_t reference(uint64_t src, uint64_t dst, unsigned width,
                          unsigned opc, unsigned r, unsigned s) {
    uint64_t wmask = ror(ones(s+1), r, width);
    uint64_t tmask = ones(((s-r) & (width-1))+1);
    uint64_t bot = ror(src, r, width) & wmask;
    if (opc == 1) bot |= dst & ~wmask;
    uint64_t top = opc == 1 ? dst : opc == 0 && ((src >> s) & 1) ? UINT64_MAX : 0;
    return ((top & ~tmask) | (bot & tmask)) & ones(width);
}
)C";
    unsigned index = 0;
    for (unsigned sf = 0; sf < 2; ++sf) {
        const unsigned width = sf ? 64 : 32;
        for (unsigned opc = 0; opc < 3; ++opc)
            for (unsigned r = 0; r < width; ++r)
                for (unsigned s = 0; s < width; ++s) {
                    const uint32_t insn = 0x13000000U | (sf << 31) | (opc << 29) |
                                          (sf << 22) | (r << 16) | (s << 10) | (1 << 5);
                    std::string emitted;
                    suyu::recomp::Translate(insn, 0, emitted);
                    std::cout << "static void f" << index++ << "(Context* c) {" << emitted << "}\n";
                }
    }
    std::cout << "static void (*functions[])(Context*) = {";
    for (unsigned i = 0; i < index; ++i) std::cout << "f" << i << ',';
    std::cout << R"C(};
int main(void) {
    const uint64_t samples[] = {0, 1, UINT64_MAX, UINT64_C(0x8000000080000000),
        UINT64_C(0x123456789abcdef0), UINT64_C(0xfedcba9876543210)};
    unsigned index = 0, checks = 0;
    for (unsigned sf = 0; sf < 2; ++sf) {
        unsigned width = sf ? 64 : 32;
        for (unsigned opc = 0; opc < 3; ++opc)
            for (unsigned r = 0; r < width; ++r)
                for (unsigned s = 0; s < width; ++s, ++index)
                    for (unsigned sample = 0; sample < sizeof(samples)/sizeof(samples[0]); ++sample) {
                        Context c = {0};
                        c.x[0] = samples[(sample+2)%6]; c.x[1] = samples[sample];
                        uint64_t expected = reference(c.x[1], c.x[0], width, opc, r, s);
                        functions[index](&c);
                        if (c.x[0] != expected || c.x[1] != samples[sample]) {
                            fprintf(stderr, "bitfield mismatch width=%u opc=%u r=%u s=%u\n", width, opc, r, s);
                            return 1;
                        }
                        ++checks;
                    }
    }
    printf("%u bitfield comparisons passed\n", checks);
    return 0;
}
)C";
}
