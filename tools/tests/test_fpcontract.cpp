// SPDX-License-Identifier: GPL-2.0-or-later
// Emits an executable test that a separate FMUL and FADD round twice, as they
// do on AArch64. The game build compiles lifted code for x86-64-v3, which has
// FMA; with floating-point contraction allowed the compiler is free to fuse
// the two into one rounding. The operands are picked so that fusing changes
// the answer: (1+2^-12)^2 = 1 + 2^-11 + 2^-24, whose 2^-24 tail is exactly half
// an ulp and rounds away, so the unfused sum is 0 and the fused one is 2^-24.
#include <cstdlib>
#include <iostream>
#include <string>
#include "core/recompiler/arm64_to_c.h"

static std::string lift(uint32_t insn) {
    std::string body;
    if (!suyu::recomp::Translate(insn, 0, body) || body.find("recomp_unhandled") != std::string::npos) {
        std::cerr << "Translate refused " << std::hex << insn << '\n';
        std::exit(1);
    }
    return body;
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
    // One function, so the multiply and the add are visible to the optimiser
    // together - the situation in a real lifted block.
    std::cout << "__attribute__((noinline)) static void block(GuestContext* c){"
              << lift(0x1E220820)   // fmul s0, s1, s2
              << lift(0x1E232800)   // fadd s0, s0, s3
              << "}\n";
    std::cout << R"C(
static void setf(GuestContext* c, int r, float f){ uint32_t u; memcpy(&u,&f,4); c->vreg[r][0]=u; c->vreg[r][1]=0; }
int main(void){
    GuestContext ctx; memset(&ctx,0,sizeof ctx);
    volatile float a = 1.0f + 0x1p-12f, m = -(1.0f + 0x1p-11f);
    setf(&ctx,1,a); setf(&ctx,2,a); setf(&ctx,3,m);
    block(&ctx);
    uint32_t u = (uint32_t)ctx.vreg[0][0]; float r; memcpy(&r,&u,4);
    if (r != 0.0f) { printf("FAIL fmul+fadd = %a, expected 0 (fused into one rounding)\n", r); return 1; }
    printf("ok\n");
    return 0;
}
)C";
}
