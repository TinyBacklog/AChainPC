// SPDX-License-Identifier: GPL-2.0-or-later
#include <iostream>
#include "core/recompiler/arm64_to_c.h"
int main() {
    std::cout << "#include <stdint.h>\n#include <stdio.h>\ntypedef struct {uint64_t x[32];} Context;\n";
    unsigned index = 0;
    for (unsigned sf = 0; sf < 2; ++sf) {
        for (unsigned shift = 0; shift < (sf ? 64 : 32); ++shift) {
            for (unsigned zr = 0; zr < 4; ++zr) {
                const uint32_t insn = 0x13800000U | (sf << 31) | (sf << 22) |
                    ((zr & 1 ? 31 : 2) << 16) | (shift << 10) | ((zr & 2 ? 31 : 1) << 5);
                std::string emitted;
                suyu::recomp::Translate(insn, 0, emitted);
                std::cout << "static void f" << index++ << "(Context* c){" << emitted << "}\n";
            }
        }
    }
    std::cout << "static void (*functions[])(Context*) = {";
    for (unsigned i = 0; i < index; ++i) std::cout << "f" << i << ',';
    std::cout << R"C(};
int main(void) {
    unsigned index = 0, checks = 0;
    const uint64_t samples[] = {0, 1, UINT64_MAX, UINT64_C(0x8000000080000000), UINT64_C(0x0123456789abcdef)};
    for (unsigned sf = 0; sf < 2; ++sf) {
        unsigned width = sf ? 64 : 32;
        uint64_t mask = sf ? UINT64_MAX : UINT32_MAX;
        for (unsigned shift = 0; shift < width; ++shift)
            for (unsigned zr = 0; zr < 4; ++zr, ++index)
                for (unsigned sample = 0; sample < 5; ++sample) {
                    Context c = {0}; c.x[1] = samples[sample]; c.x[2] = samples[(sample+2)%5];
                    __uint128_t high = zr & 2 ? 0 : c.x[1] & mask;
                    __uint128_t low = zr & 1 ? 0 : c.x[2] & mask;
                    uint64_t expected = (uint64_t)(((high << width) | low) >> shift) & mask;
                    functions[index](&c);
                    if (expected != c.x[0]) return 1;
                    ++checks;
                }
    }
    printf("%u EXTR comparisons passed\n", checks);
}
)C";
}
