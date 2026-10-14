// SPDX-License-Identifier: GPL-2.0-or-later
// Emits an executable test of the CRC32 and CRC32C accumulate instructions.
#include <iostream>
#include <string>
#include "core/recompiler/arm64_to_c.h"

int main() {
    std::cout << R"C(
#include <stdint.h>
#include <stdio.h>
typedef struct GuestContext { uint64_t x[32]; } GuestContext;
/* Reference model taken straight from the architectural description: reverse
   the bits, reduce modulo the forward polynomial, reverse back. It shares no
   code with the emitted reflected byte loop. */
static uint64_t bitrev(uint64_t v, unsigned n) {
    uint64_t r = 0;
    for (unsigned i = 0; i < n; ++i) if ((v >> i) & 1) r |= UINT64_C(1) << (n - 1 - i);
    return r;
}
static uint32_t reference(uint32_t acc, uint64_t val, unsigned size, uint32_t poly) {
    __uint128_t data = ((__uint128_t)bitrev(acc, 32) << size) ^
                       ((__uint128_t)bitrev(val, size) << 32);
    for (int i = (int)size + 31; i >= 32; --i)
        if ((data >> i) & 1)
            data ^= ((__uint128_t)(((__uint128_t)1 << 32) | poly)) << (i - 32);
    return (uint32_t)bitrev((uint64_t)(data & UINT32_MAX), 32);
}
)C";

    // Rn = 1 (the running accumulator), Rm = 2 (the data), Rd = 0.
    unsigned slot = 0;
    std::string dispatch;
    for (unsigned opcode = 16; opcode < 24; ++opcode) {
        const unsigned sz = opcode & 3;
        const unsigned sf = (sz == 3) ? 1u : 0u;
        const uint32_t insn = 0x1AC00000u | (sf << 31) | (2u << 16) | (opcode << 10) |
                              (1u << 5) | 0u;
        std::string body;
        if (!suyu::recomp::Translate(insn, 0, body) ||
            body.find("recomp_unhandled") != std::string::npos) {
            std::cerr << "not translated: " << std::hex << insn << '\n';
            return 1;
        }
        std::cout << "static void f" << slot << "(GuestContext* c){" << body << "}\n";
        dispatch += "f" + std::to_string(slot) + ",";
        ++slot;
    }
    std::cout << "static void (*functions[])(GuestContext*) = {" << dispatch << "};\n";
    std::cout << R"C(
int main(void) {
    static const uint64_t accs[] = {0, 1, 0xFFFFFFFFULL, 0x12345678ULL, 0xDEADBEEFULL};
    static const uint64_t vals[] = {0, 1, 0xFFULL, 0xA5A5A5A5A5A5A5A5ULL,
                                    0x0123456789ABCDEFULL, UINT64_MAX};
    unsigned checks = 0, slot = 0;
    for (unsigned opcode = 16; opcode < 24; ++opcode, ++slot) {
        const unsigned size = 8u << (opcode & 3);
        const uint32_t poly = (opcode & 4) ? 0x1EDC6F41u : 0x04C11DB7u;
        for (unsigned a = 0; a < 5; ++a)
        for (unsigned v = 0; v < 6; ++v) {
            GuestContext c;
            for (int i = 0; i < 32; ++i) c.x[i] = 0;
            c.x[1] = accs[a];
            c.x[2] = vals[v];
            const uint64_t masked = size == 64 ? vals[v] : (vals[v] & ((UINT64_C(1) << size) - 1));
            const uint32_t want = reference((uint32_t)accs[a], masked, size, poly);
            functions[slot](&c);
            if (c.x[0] != (uint64_t)want) {
                printf("FAIL opcode=%u acc=%llx val=%llx got=%llx want=%x\n", opcode,
                       (unsigned long long)accs[a], (unsigned long long)vals[v],
                       (unsigned long long)c.x[0], want);
                return 1;
            }
            ++checks;
        }
    }
    if (slot != sizeof functions / sizeof functions[0]) return 1;
    printf("%u CRC32 comparisons passed\n", checks);
    return 0;
}
)C";
    return 0;
}
