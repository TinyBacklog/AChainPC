// SPDX-License-Identifier: GPL-2.0-or-later
// Emits an executable test of runtime-toggled patches (TranslatePatched).
//
// A patched instruction is lifted twice and chosen at run time by acre_fps60, so
// the only acceptable behaviour with the flag off is the original instruction,
// bit for bit, and with it on the replacement (or the hook). A wrong expected
// word must stop the export rather than lift code nobody checked.
#include <cassert>
#include <iostream>
#include <stdexcept>
#include <string>
#include "core/recompiler/arm64_to_c.h"

using suyu::recomp::Patch;

static std::string Block(const char* name, uint32_t word, uint64_t pc, const Patch& patch) {
    std::string body;
    bool open = true;
    suyu::recomp::TranslatePatched(word, pc, &patch, body, open);
    std::string out = std::string("static void ") + name + "(GuestContext* c){\n" + body;
    if (open) {
        char tail[64];
        snprintf(tail, sizeof tail, "    c->pc=g_module_base+0x%llxULL; return;\n",
                 (unsigned long long)(pc + 4));
        out += tail;
    }
    return out + "}\n";
}

int main() {
    // A mismatching expected word must refuse to lift.
    {
        Patch bad{0x10, 0xdeadbeef, Patch::Kind::Word, 0x52800021, {}};
        std::string body;
        bool open = true;
        bool threw = false;
        try {
            suyu::recomp::TranslatePatched(0x52800041, 0x10, &bad, body, open);
        } catch (const std::runtime_error& e) {
            threw = std::string(e.what()).find("0x10") != std::string::npos;
        }
        if (!threw) {
            std::cerr << "mismatched expected word was not rejected\n";
            return 1;
        }
    }

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
volatile int acre_fps60 = 0;
static int test_hook(GuestContext* c){
    if (c->x[8] == 0) { c->x[0] = 42; c->pc = g_module_base + 0x100; return 1; }
    return 0;
}
)C";
    // mov w1,#2 -> mov w1,#1
    std::cout << Block("word_plain", 0x52800041, 0x0, Patch{0x0, 0x52800041, Patch::Kind::Word, 0x52800021, {}});
    // add x0,x0,#1 -> cbz x0,+8 (a replacement that ends the block)
    std::cout << Block("word_branch", 0x91000400, 0x0, Patch{0x0, 0x91000400, Patch::Kind::Word, 0xb4000040, {}});
    // hook before mov w1,#2
    std::cout << Block("hooked", 0x52800041, 0x0, Patch{0x0, 0x52800041, Patch::Kind::Hook, 0, "test_hook"});
    std::cout << R"C(
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
int main(void) {
    GuestContext c;
    memset(&c, 0, sizeof c); acre_fps60 = 0; word_plain(&c);  CHECK(c.x[1] == 2); CHECK(c.pc == 4);
    memset(&c, 0, sizeof c); acre_fps60 = 1; word_plain(&c);  CHECK(c.x[1] == 1); CHECK(c.pc == 4);

    memset(&c, 0, sizeof c); acre_fps60 = 0; word_branch(&c); CHECK(c.x[0] == 1); CHECK(c.pc == 4);
    memset(&c, 0, sizeof c); acre_fps60 = 1; word_branch(&c); CHECK(c.x[0] == 0); CHECK(c.pc == 8);
    memset(&c, 0, sizeof c); c.x[0] = 5; acre_fps60 = 1; word_branch(&c); CHECK(c.x[0] == 5); CHECK(c.pc == 4);

    memset(&c, 0, sizeof c); acre_fps60 = 0; hooked(&c); CHECK(c.x[0] == 0); CHECK(c.x[1] == 2); CHECK(c.pc == 4);
    memset(&c, 0, sizeof c); acre_fps60 = 1; hooked(&c); CHECK(c.x[0] == 42); CHECK(c.x[1] == 0); CHECK(c.pc == 0x100);
    memset(&c, 0, sizeof c); c.x[8] = 1; acre_fps60 = 1; hooked(&c); CHECK(c.x[0] == 0); CHECK(c.x[1] == 2); CHECK(c.pc == 4);
    if (fails) return 1;
    printf("patch: ok\n");
    return 0;
}
)C";
    return 0;
}
