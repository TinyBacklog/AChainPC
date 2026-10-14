// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Header-only AArch64 -> portable C static recompiler engine, shared by the standalone
// tools/static_recompiler CLI and the in-app game export feature.
//
// It decodes a subset of user-mode AArch64 and emits C against a GuestContext (N64Recomp-style).
// Every instruction either translates to native C or emits a runtime fallback, so the generated
// project ALWAYS builds into a native binary (Windows .exe / Linux+BSD ELF / macOS Mach-O) or can
// be emitted as plain C source. A full game additionally needs suyu's HLE/GPU runtime, wired in via
// the generated runtime's recomp_svc()/MMIO hooks.

#pragma once

#include <array>
#include "core/arm/recomp/recomp_identity.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

namespace suyu::recomp {

using u8 = uint8_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s32 = int32_t;
using s64 = int64_t;

// Output paths reach this engine as UTF-8 (the frontend hands over
// QString::toStdString()), but the narrow char overloads of _mkdir/mkdir and
// std::ofstream interpret their argument in the process code page - CP-1252 on
// a stock Windows install. A title whose name carries a non-ASCII character
// ("Pokemon Sword" reads as "Pokémon Sword" once the name comes from the
// ROM's own NACP) therefore had every directory creation and every file open
// silently fail, leaving empty module directories and a build that could not
// configure. Route both through std::filesystem::path, which is constructed
// from the UTF-8 bytes explicitly and uses the wide OS entry points.
inline std::filesystem::path Utf8Path(const std::string& s) {
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

struct Block {
    u64 vaddr;
    u32 size;
    u32 count;
    bool is_entry;
};

// A game patch applied while lifting. The emitted code picks the original or the
// patched behaviour at run time from `acre_fps60`, so one export serves both
// settings. Word: the instruction at `pc` is replaced by `replacement`. Hook: the
// C function `hook(GuestContext*)` runs first; returning 1 means it has set the
// PC and the block ends there, 0 means the original instruction runs as usual.
struct Patch {
    enum class Kind { Word, Hook };
    u64 pc;
    u32 expected;
    Kind kind;
    u32 replacement;
    std::string hook;
};

inline bool IsTerminator(u32 i) {
    if ((i & 0xFC000000) == 0x14000000) return true; // B
    if ((i & 0xFC000000) == 0x94000000) return true; // BL
    if ((i & 0xFFFFFC1F) == 0xD61F0000) return true; // BR
    if ((i & 0xFFFFFC1F) == 0xD63F0000) return true; // BLR
    if ((i & 0xFFFFFC1F) == 0xD65F0000) return true; // RET
    if ((i & 0x7F000000) == 0x34000000) return true; // CBZ
    if ((i & 0x7F000000) == 0x35000000) return true; // CBNZ
    if ((i & 0x7F000000) == 0x36000000) return true; // TBZ
    if ((i & 0x7F000000) == 0x37000000) return true; // TBNZ
    if ((i & 0xFF000010) == 0x54000000) return true; // B.cond
    if ((i & 0xFFE0001F) == 0xD4000001) return true; // SVC
    return false;
}

// Statically known target of a direct branch, if the instruction has one.
//
// This has to cover every form whose target the translator bakes in, not just
// the unconditional imm26 pair: a conditional branch's target is equally
// static, and it is overwhelmingly the common case for loop back-edges. Leaving
// B.cond/CBZ/CBNZ/TBZ/TBNZ out here meant their targets never began a block, so
// the generated code would set pc to an address that recomp_lookup - which
// matches block starts exactly - could not resolve, and execution died with
// "No recompiled block at PC" at the top of the first loop it entered.
inline bool DirectBranchTarget(u32 i, u64 pc, u64& out) {
    if ((i & 0xFC000000) == 0x14000000 || (i & 0xFC000000) == 0x94000000) { // B / BL
        s32 imm26 = (s32)(i << 6) >> 6;
        out = pc + (s64)imm26 * 4;
        return true;
    }
    if ((i & 0xFF000010) == 0x54000000 ||    // B.cond
        (i & 0x7E000000) == 0x34000000) {    // CBZ / CBNZ
        s64 imm19 = ((s32)(((i >> 5) & 0x7FFFF) << 13) >> 13);
        out = pc + imm19 * 4;
        return true;
    }
    if ((i & 0x7E000000) == 0x36000000) {    // TBZ / TBNZ
        s64 imm14 = ((s32)(((i >> 5) & 0x3FFF) << 18) >> 18);
        out = pc + imm14 * 4;
        return true;
    }
    return false;
}

// Function addresses the code builds for itself with ADRP+ADD.
//
// A callback handed to the OS - a thread entry above all - is never branched to
// from inside .text and never appears in a relocation either: the compiler just
// materialises its address into a register (adrp xN, page; add xN, xN, #lo12)
// and passes it to svcCreateThread. Without seeding those, every thread the
// game starts begins at an address no block covers and drops straight to the
// interpreter for its whole life, which is most of the "No recompiled block at
// PC" traffic in a real run.
//
// Deliberately loose: any ADRP+ADD landing in .text becomes a root. A pair that
// was really computing a data address only costs one extra block boundary.
inline void CollectAdrpAddTargets(const u8* text, size_t n_bytes, u64 base,
                                  std::vector<u64>& out) {
    const u32 n = static_cast<u32>(n_bytes / 4);
    const u32* p = reinterpret_cast<const u32*>(text);
    // Last ADRP seen per destination register, as a page address; ~0 means the
    // register no longer holds one.
    std::array<u64, 32> page{};
    page.fill(~0ULL);
    for (u32 i = 0; i < n; ++i) {
        const u32 insn = p[i];
        if ((insn & 0x9F000000) == 0x90000000) { // ADRP
            const u32 rd = insn & 31;
            s64 immhi = static_cast<s32>((((insn >> 5) & 0x7FFFF) << 13)) >> 13;
            const u32 immlo = (insn >> 29) & 3;
            page[rd] = ((base + static_cast<u64>(i) * 4) & ~0xFFFULL) +
                       static_cast<u64>(((immhi << 2) | immlo) << 12);
        } else if ((insn & 0xFFC00000) == 0x91000000) { // ADD (immediate, 64-bit, LSL #0)
            const u32 rd = insn & 31, rn = (insn >> 5) & 31;
            const u32 imm12 = (insn >> 10) & 0xFFF;
            if (page[rn] != ~0ULL) {
                const u64 target = page[rn] + imm12;
                if (target >= base && (target - base) / 4 < n && (target & 3) == 0) {
                    out.push_back(target);
                }
            }
            if (rd != rn) {
                page[rd] = ~0ULL;
            }
        } else {
            // Any other write to a register invalidates the page it held. Only
            // the common destination encodings are decoded here; missing one
            // can add a stale root, never remove a real one.
            const u32 rd = insn & 31;
            page[rd] = ~0ULL;
        }
    }
}

inline std::vector<Block> DiscoverBlocks(const u8* text, size_t n_bytes, u64 base,
                                         u64 entry_pc = 0,
                                         const std::vector<u64>* extra_roots = nullptr) {
    const u32 n = (u32)(n_bytes / 4);
    if (n == 0) return {};
    std::vector<bool> start(n, false);
    start[0] = true;
    // The module entry has to begin a block in its own right. Nothing branches
    // to it from inside the image, and for a real NSO it isn't offset 0
    // either - .text opens with a MOD0 header - so without this the entry
    // address falls in the middle of some other block and lookup fails.
    if (entry_pc > base && (entry_pc - base) / 4 < n) {
        start[(u32)((entry_pc - base) / 4)] = true;
    }
    // Exported dynsym addresses (functions like nn::init::Start, only ever
    // reached indirectly via another module's resolved GOT/PLT entry, never
    // by a direct branch inside this module's own .text). Without seeding
    // these as roots too, a function preceded by alignment padding rather
    // than a terminator falls mid-block and the runtime dispatcher can never
    // resolve a call landing exactly on its real entry address.
    if (extra_roots) {
        for (u64 addr : *extra_roots) {
            if (addr >= base && (addr - base) / 4 < n) {
                start[(u32)((addr - base) / 4)] = true;
            }
        }
    }
    {
        std::vector<u64> computed;
        CollectAdrpAddTargets(text, n_bytes, base, computed);
        for (u64 addr : computed) {
            start[(u32)((addr - base) / 4)] = true;
        }
    }
    const u32* p = reinterpret_cast<const u32*>(text);
    for (u32 i = 0; i < n; ++i) {
        const u32 insn = p[i];
        const u64 pc = base + (u64)i * 4;
        if (IsTerminator(insn)) {
            if (i + 1 < n) start[i + 1] = true;
            u64 t = 0;
            if (DirectBranchTarget(insn, pc, t) && t >= base && (t - base) / 4 < n)
                start[(u32)((t - base) / 4)] = true;
        }
    }
    std::vector<Block> blocks;
    u32 s = 0;
    for (u32 i = 1; i <= n; ++i) {
        if (i == n || start[i]) {
            blocks.push_back(Block{base + (u64)s * 4, (i - s) * 4, i - s, s == 0});
            s = i;
        }
    }
    return blocks;
}

// ARM's logical-immediate encoding: N:immr:imms describe a repeating bit
// pattern rather than a literal value. This is the standard DecodeBitMasks
// from the architecture reference, restricted to the immediate (non-tested)
// result. Returns false for the reserved encodings.
inline bool DecodeBitMasks(u32 N, u32 imms, u32 immr, bool is64, u64& out) {
    if (!is64 && N) return false;
    // len = index of the highest set bit of (N:~imms), computed portably.
    const u32 bits = (N << 6) | ((~imms) & 0x3F);
    if (bits == 0) return false;
    u32 len = 0;
    for (u32 t = bits; t > 1; t >>= 1) ++len;
    if (len < 1) return false;
    const u32 esize = 1u << len;
    if (esize > (is64 ? 64u : 32u)) return false;
    const u32 levels = esize - 1;
    const u32 s = imms & levels;
    const u32 r = immr & levels;
    if (s == levels) return false;   // reserved
    u64 welem = (s + 1 >= 64) ? ~0ULL : ((1ULL << (s + 1)) - 1);
    // Rotate right within the element, then replicate to the register width.
    // The rotate applies at every element size, 64 included: skipping it there
    // (to dodge the undefined `welem << 64` when r is 0) silently turned every
    // 64-bit rotated mask into its unrotated form - so "and x8, x8, #~0xF",
    // the standard align-down, decoded as `& 0x0FFFFFFFFFFFFFFF` and left the
    // pointer unaligned instead. Guard r == 0 explicitly instead.
    u64 elem;
    if (r == 0) {
        elem = welem;
    } else if (esize >= 64) {
        elem = (welem >> r) | (welem << (64 - r));
    } else {
        elem = ((welem >> r) | (welem << (esize - r))) & ((1ULL << esize) - 1);
    }
    u64 result = 0;
    for (u32 i = 0; i < (is64 ? 64u : 32u); i += esize) result |= elem << i;
    if (!is64) result &= 0xFFFFFFFFULL;
    out = result;
    return true;
}

inline std::string Xz(u32 r) {
    return r == 31 ? std::string("(uint64_t)0") : ("c->x[" + std::to_string(r) + "]");
}
inline std::string Wz(u32 r) {
    return r == 31 ? std::string("(uint32_t)0") : ("(uint32_t)c->x[" + std::to_string(r) + "]");
}

/// Register 31 as the stack pointer rather than the zero register.
///
/// AArch64 spells both with the same encoding and the meaning depends on the
/// instruction: ADD/SUB with an immediate or an extended register read it as
/// SP, while the shifted-register and logical forms read it as XZR. Using the
/// zero-register spelling everywhere makes every function prologue -
/// "sub sp, sp, #N", "add x29, sp, #N" - compute from zero, so the frame lands
/// at a tiny address and every local access writes into unmapped memory near
/// null.
inline std::string Xsp(u32 r) {
    return "c->x[" + std::to_string(r) + "]";
}

// Append C for one instruction. Returns false if the instruction terminates the block.
inline bool Translate(u32 i, u64 pc, std::string& out) {
    char buf[256];
    // Appends in place. Taking a string_view and appending piecewise (rather
    // than `out += "    " + s + "\n"`) matters at scale: that expression built
    // two temporary std::strings per emitted line, and a full title is tens of
    // millions of lines - it was the single largest cost in AOT translation.
    auto put = [&](std::string_view s) {
        out.append("    ", 4);
        out.append(s);
        out.push_back('\n');
    };
    const u64 next = pc + 4;

    if ((i & 0xFFFFF01F) == 0xD503201F) { put("/* nop/hint */"); return true; }

    if ((i & 0xfffff0ffU) == 0xd503305fU) { // CLREX
        snprintf(buf, sizeof buf,
                 "recomp_exclusive(c,0x%08xU,g_module_base+0x%llxULL); if(c->halted) return;",
                 i, (unsigned long long)pc);
        put(buf);
        return true;
    }
    // DSB/DMB/ISB order accesses through the host memory bridge.
    if ((i & 0xFFFFF01F) == 0xD503301F) { put("recomp_barrier();"); return true; }

    if ((i & 0x1F800000) == 0x12800000) { // MOVZ/MOVN/MOVK
        u32 sf = i >> 31, opc = (i >> 29) & 3, hw = (i >> 21) & 3, imm16 = (i >> 5) & 0xFFFF, rd = i & 31;
        if (rd != 31) {
            u64 shift = (u64)hw * 16;
            if (opc == 2) {
                snprintf(buf, sizeof buf, "c->x[%u] = 0x%llxULL;", rd, (unsigned long long)((u64)imm16 << shift));
                put(buf);
            } else if (opc == 0) {
                u64 v = ~((u64)imm16 << shift); if (!sf) v &= 0xFFFFFFFF;
                snprintf(buf, sizeof buf, "c->x[%u] = 0x%llxULL;", rd, (unsigned long long)v); put(buf);
            } else if (opc == 3) {
                snprintf(buf, sizeof buf, "c->x[%u] = (c->x[%u] & ~(0xFFFFULL<<%llu)) | (0x%xULL<<%llu);",
                         rd, rd, (unsigned long long)shift, imm16, (unsigned long long)shift); put(buf);
            }
            if (!sf) { snprintf(buf, sizeof buf, "c->x[%u] &= 0xFFFFFFFFULL;", rd); put(buf); }
        }
        return true;
    }

    if ((i & 0x1F000000) == 0x11000000) { // ADD/SUB immediate
        u32 sf = i >> 31, op = (i >> 30) & 1, S = (i >> 29) & 1, sh = (i >> 22) & 1;
        u32 imm12 = (i >> 10) & 0xFFF, rn = (i >> 5) & 31, rd = i & 31;
        u64 imm = sh ? ((u64)imm12 << 12) : imm12;
        snprintf(buf, sizeof buf, "{ uint64_t _a=c->x[%u], _b=%lluULL; uint64_t _r=%s; ", rn,
                 (unsigned long long)imm, op ? "_a-_b" : "_a+_b");
        std::string s = buf;
        if (!sf) s += "_r&=0xFFFFFFFFULL; ";
        // With the flag-setting form, register 31 as the destination is the
        // zero register - that encoding is CMP - so the result is dropped.
        // Without it, register 31 is SP and the write is real.
        if (!(rd == 31 && S)) s += "c->x[" + std::to_string(rd) + "]=_r; ";
        if (S) s += "recomp_set_flags(c," + std::string(op ? "1" : "0") + ",_a,_b,_r," + (sf ? "1" : "0") + "); ";
        s += "}";
        // Register 31 is SP here, not the zero register, so a write to it is
        // real and must not be discarded: dropping it throws away every
        // "sub sp, sp, #N" that opens a stack frame.
        put(s);
        return true;
    }

    // Build a shifted-register operand. LSR/ASR/ROR all used to collapse to a
    // plain ">>" on a uint64_t, which zero-fills - so ASR lost the sign (the
    // "bic w0, w0, w0, asr #31" max(x,0) idiom produced ~0 instead of 0) and
    // ROR dropped the wrapped-around bits entirely (breaking, among other
    // things, software CRC32). The 32-bit forms also have to be narrowed
    // before shifting, whatever the shift amount, because the register may
    // still carry high garbage from an earlier 64-bit write.
    auto shifted_operand = [](const std::string& v, u32 shift, u32 imm6, u32 sf) -> std::string {
        char sb[256];
        if (!sf) {
            const std::string w = "((uint32_t)(" + v + "))";
            switch (shift) {
            case 0: snprintf(sb, sizeof sb, "((uint64_t)(uint32_t)(%s << %u))", w.c_str(), imm6); break;
            case 1: snprintf(sb, sizeof sb, "((uint64_t)(%s >> %u))", w.c_str(), imm6); break;
            case 2: snprintf(sb, sizeof sb, "((uint64_t)(uint32_t)((int32_t)%s >> %u))", w.c_str(), imm6); break;
            default:
                if (imm6 == 0) { snprintf(sb, sizeof sb, "((uint64_t)%s)", w.c_str()); }
                else { snprintf(sb, sizeof sb, "((uint64_t)(uint32_t)((%s >> %u) | (%s << %u)))", w.c_str(), imm6, w.c_str(), 32 - imm6); }
                break;
            }
        } else {
            switch (shift) {
            case 0: snprintf(sb, sizeof sb, "((%s) << %u)", v.c_str(), imm6); break;
            case 1: snprintf(sb, sizeof sb, "((%s) >> %u)", v.c_str(), imm6); break;
            case 2: snprintf(sb, sizeof sb, "((uint64_t)((int64_t)(%s) >> %u))", v.c_str(), imm6); break;
            default:
                if (imm6 == 0) { snprintf(sb, sizeof sb, "(%s)", v.c_str()); }
                else { snprintf(sb, sizeof sb, "(((%s) >> %u) | ((%s) << %u))", v.c_str(), imm6, v.c_str(), 64 - imm6); }
                break;
            }
        }
        return sb;
    };

    if ((i & 0x1F000000) == 0x0A000000) { // logical shifted register
        u32 sf = i >> 31, opc = (i >> 29) & 3, rm = (i >> 16) & 31, rn = (i >> 5) & 31, rd = i & 31;
        u32 shift = (i >> 22) & 3, imm6 = (i >> 10) & 0x3F, N = (i >> 21) & 1;
        std::string rmv = shifted_operand(Xz(rm), shift, imm6, sf);
        std::string a = Xz(rn);
        const char* lop = opc == 0 ? "&" : opc == 1 ? "|" : opc == 2 ? "^" : "&";
        std::string expr = N ? ("(" + a + " " + lop + " ~" + rmv + ")") : ("(" + a + " " + lop + " " + rmv + ")");
        // opc==3 is ANDS/BICS: it sets the flags, and with rd==31 it is TST,
        // whose only effect IS the flag update. Gating the whole instruction on
        // rd != 31 discarded every TST, leaving the following b.cond to branch
        // on whatever flags happened to be left over from an earlier compare.
        std::string s = "{ uint64_t _r = " + expr + "; ";
        if (!sf) s += "_r &= 0xFFFFFFFFULL; ";
        if (rd != 31) s += "c->x[" + std::to_string(rd) + "] = _r; ";
        if (opc == 3) s += "recomp_set_flags(c,0,_r,0,_r," + std::string(sf ? "1" : "0") + "); ";
        s += "}";
        put(s);
        return true;
    }

    // ADC/ADCS/SBC/SBCS: sf op S 11010000 Rm 000000 Rn Rd. This has to be
    // tested before the ADD/SUB shifted-register arm below, whose wider mask
    // would otherwise swallow it. Without this whole class the SDK's
    // multi-precision arithmetic and its "materialise the carry flag" idiom
    // (adcs x0, xzr, xzr) fall through to recomp_unhandled.
    if ((i & 0x1FE0FC00) == 0x1A000000) {
        const u32 sf = i >> 31, op = (i >> 30) & 1, S = (i >> 29) & 1;
        const u32 rm = (i >> 16) & 31, rn = (i >> 5) & 31, rd = i & 31;
        // Unlike ADD/SUB immediate, register 31 is the zero register in every
        // position here - there is no SP form - so a write to it is discarded
        // whether or not the instruction sets flags.
        std::string s = "{ uint64_t _a=" + Xz(rn) + ", _b=" + Xz(rm) + "; ";
        // SBC is AddWithCarry(Rn, NOT(Rm), C); ADC is AddWithCarry(Rn, Rm, C).
        s += "uint64_t _r=recomp_add_with_carry(c,_a," + std::string(op ? "~_b" : "_b") +
              ",c->c," + std::string(sf ? "1" : "0") + "," + std::string(S ? "1" : "0") + "); ";
        if (rd != 31) s += "c->x[" + std::to_string(rd) + "]=_r; ";
        else s += "(void)_r; ";
        s += "}";
        put(s);
        return true;
    }

    if ((i & 0x1F200000) == 0x0B000000) { // ADD/SUB shifted register
        u32 sf = i >> 31, op = (i >> 30) & 1, S = (i >> 29) & 1, shift = (i >> 22) & 3, rm = (i >> 16) & 31, imm6 = (i >> 10) & 0x3F, rn = (i >> 5) & 31, rd = i & 31;
        // ROR is reserved for ADD/SUB shifted register - decoding it as a shift
        // would silently invent an instruction the CPU does not have.
        if (shift == 3) {
            snprintf(buf, sizeof buf,
                     "recomp_unhandled(c,0x%08xU,g_module_base+0x%llxULL); if(c->halted) return;", i,
                     (unsigned long long)pc);
            put(buf);
            return true;
        }
        std::string rmv = shifted_operand(Xz(rm), shift, imm6, sf);
        std::string a = Xz(rn);
        snprintf(buf, sizeof buf, "{ uint64_t _a=%s,_b=%s,_r=%s; ", a.c_str(), rmv.c_str(), op ? "_a-_b" : "_a+_b");
        std::string s = buf; if (!sf) s += "_r&=0xFFFFFFFFULL; ";
        if (rd != 31) s += "c->x[" + std::to_string(rd) + "]=_r; ";
        if (S) s += "recomp_set_flags(c," + std::string(op ? "1" : "0") + ",_a,_b,_r," + (sf ? "1" : "0") + "); ";
        s += "}"; put(s); return true;
    }

    if ((i & 0x1F000000) == 0x10000000) { // ADR/ADRP
        u32 op = i >> 31, rd = i & 31; s64 immhi = (s32)(((i >> 5) & 0x7FFFF) << 13) >> 13; u32 immlo = (i >> 29) & 3;
        if (rd != 31) {
            // These produce data pointers the guest then dereferences, so they
            // have to be real addresses. Everything the static pass knows is
            // module-relative, so the module's load base is added at run time -
            // without it every computed pointer lands near null.
            if (op) { u64 b = (pc & ~0xFFFULL); s64 imm = ((immhi << 2) | immlo) << 12; snprintf(buf, sizeof buf, "c->x[%u]=g_module_base + 0x%llxULL + (int64_t)%lld;", rd, (unsigned long long)b, (long long)imm); }
            else { s64 imm = (immhi << 2) | immlo; snprintf(buf, sizeof buf, "c->x[%u]=g_module_base + 0x%llxULL + (int64_t)%lld;", rd, (unsigned long long)pc, (long long)imm); }
            put(buf);
        }
        return true;
    }

    // LDR/STR immediate unsigned offset. Bit 26 is the V bit: it selects the
    // SIMD/FP register file rather than the general registers. Leaving it out
    // of the mask made every "LDR s0, [x1, #8]" compile into an integer load
    // of x0 - silently wrong code rather than an honest fallback. The SIMD
    // form is handled separately further down.
    if ((i & 0x3F000000) == 0x39000000) {
        u32 size = (i >> 30) & 3, opc = (i >> 22) & 3, imm12 = (i >> 10) & 0xFFF, rn = (i >> 5) & 31, rt = i & 31;
        u64 off = (u64)imm12 << size;
        std::string addr = "c->x[" + std::to_string(rn) + "] + " + std::to_string(off);
        const char* ty = size == 0 ? "8" : size == 1 ? "16" : size == 2 ? "32" : "64";
        // opc: 00 store, 01 load (zero-extend), 10 load signed to 64-bit,
        // 11 load signed to 32-bit. Testing (opc & 1) got this wrong in both
        // directions - opc 11 loaded without sign extension, and opc 10 (a
        // *load*) fell into the store branch and wrote to memory instead.
        if (opc == 0) {
            snprintf(buf, sizeof buf, "recomp_store%s(c,%s,%s);", ty, addr.c_str(), Xz(rt).c_str());
            put(buf);
        } else if (opc == 1) {
            if (rt != 31) {
                snprintf(buf, sizeof buf, "c->x[%u]=recomp_load%s(c,%s);", rt, ty, addr.c_str());
                put(buf);
            }
        } else if (size < 3) {
            // Signed load: sign-extend from the accessed width.
            const char* st = size == 0 ? "int8_t" : size == 1 ? "int16_t" : "int32_t";
            if (rt != 31) {
                std::string s = "{ uint64_t _r = (uint64_t)(int64_t)(" + std::string(st) +
                                ")recomp_load" + ty + "(c," + addr + "); ";
                if (opc == 3) s += "_r &= 0xFFFFFFFFULL; ";   // 32-bit destination
                s += "c->x[" + std::to_string(rt) + "] = _r; }";
                put(s);
            }
        } else if (opc == 2) {
            // size==3 with opc==2 is PRFM, a prefetch hint. Rt is not a
            // register here but the prefetch operation, and nothing in this
            // backend models a cache, so the whole instruction is a no-op.
            put("/* prfm */");
        } else {
            // size==3 with opc==3 is not a defined load/store here.
            snprintf(buf, sizeof buf,
                     "recomp_unhandled(c,0x%08xU,g_module_base+0x%llxULL); if(c->halted) return;", i,
                     (unsigned long long)pc);
            put(buf);
            return true;
        }
        return true;
    }

    u64 t = 0;
    // Only the unconditional forms here: DirectBranchTarget also decodes
    // B.cond/CBZ/TBZ for block discovery, but those have their own translations
    // further down and must not be turned into unconditional jumps.
    if (((i & 0xFC000000) == 0x14000000 || (i & 0xFC000000) == 0x94000000) &&
        DirectBranchTarget(i, pc, t)) {
        if ((i & 0xFC000000) == 0x94000000) { snprintf(buf, sizeof buf, "c->x[30]=g_module_base+0x%llxULL;", (unsigned long long)next); put(buf); }
        snprintf(buf, sizeof buf, "c->pc=g_module_base+0x%llxULL; return;", (unsigned long long)t); put(buf); return false;
    }
    if ((i & 0xFFFFFC1F) == 0xD65F0000) { put("c->pc=c->x[30]; return; /* RET */"); return false; }
    if ((i & 0xFFFFFC1F) == 0xD61F0000) { u32 rn = (i >> 5) & 31; snprintf(buf, sizeof buf, "c->pc=c->x[%u]; return; /* BR */", rn); put(buf); return false; }
    if ((i & 0xFFFFFC1F) == 0xD63F0000) { u32 rn = (i >> 5) & 31; snprintf(buf, sizeof buf, "c->x[30]=g_module_base+0x%llxULL; c->pc=c->x[%u]; return; /* BLR */", (unsigned long long)next, rn); put(buf); return false; }
    if ((i & 0xFF000010) == 0x54000000) { s64 off = ((s32)((i >> 5) << 13) >> 13); u64 tt = pc + off * 4; u32 cond = i & 15; snprintf(buf, sizeof buf, "if (recomp_cond(c,%u)) { c->pc=g_module_base+0x%llxULL; } else { c->pc=g_module_base+0x%llxULL; } return;", cond, (unsigned long long)tt, (unsigned long long)next); put(buf); return false; }
    if ((i & 0x7E000000) == 0x34000000) { u32 sf = i >> 31; bool nz = (i >> 24) & 1; u32 rt = i & 31; s64 off = ((s32)(((i >> 5) & 0x7FFFF) << 13) >> 13); u64 tt = pc + off * 4; std::string v = sf ? Xz(rt) : Wz(rt); snprintf(buf, sizeof buf, "if ((%s)%s0) { c->pc=g_module_base+0x%llxULL; } else { c->pc=g_module_base+0x%llxULL; } return;", v.c_str(), nz ? "!=" : "==", (unsigned long long)tt, (unsigned long long)next); put(buf); return false; }
    if ((i & 0x7E000000) == 0x36000000) { bool nz = (i >> 24) & 1; u32 b = ((i >> 31) << 5) | ((i >> 19) & 31); u32 rt = i & 31; s64 off = ((s32)(((i >> 5) & 0x3FFF) << 18) >> 18); u64 tt = pc + off * 4; snprintf(buf, sizeof buf, "if (((%s>>%u)&1)%s0) { c->pc=g_module_base+0x%llxULL; } else { c->pc=g_module_base+0x%llxULL; } return;", Xz(rt).c_str(), b, nz ? "!=" : "==", (unsigned long long)tt, (unsigned long long)next); put(buf); return false; }
    if ((i & 0xFFE0001F) == 0xD4000001) { u32 imm = (i >> 5) & 0xFFFF; snprintf(buf, sizeof buf, "c->pc=g_module_base+0x%llxULL; c->pending_svc=%uULL; recomp_svc(c,%u); return;", (unsigned long long)next, imm, imm); put(buf); return false; }

    // STP/LDP - load/store pair. Every non-leaf AArch64 function opens and
    // closes with these, so without them a real game stops at its first
    // prologue. Covers the signed-offset, pre-index and post-index forms for
    // both 32- and 64-bit operands.
    // Bit 26 (V) must be clear here; the SIMD/FP pair form is handled below.
    if ((i & 0x3E000000) == 0x28000000) {
        const u32 opc = i >> 30;            // 0 = 32-bit, 2 = 64-bit
        const bool is_load = (i >> 22) & 1;
        const u32 mode = (i >> 23) & 3;     // 1 post-index, 2 signed offset, 3 pre-index
        const u32 rt2 = (i >> 10) & 31, rn = (i >> 5) & 31, rt = i & 31;
        s32 imm7 = (s32)((i >> 15) & 0x7F);
        if (imm7 & 0x40) imm7 |= ~0x7F;     // sign-extend 7 bits
        // LDPSW: opc 01, load only. It reads a pair of 32-bit words and
        // sign-extends each to 64 bits, so it cannot share the plain 32-bit
        // path above, which zero-extends. The offset is scaled by 4, the
        // access size, not by the 8-byte destination width.
        if (opc == 1 && is_load && mode >= 1 && mode <= 3) {
            const s64 off = (s64)imm7 * 4;
            std::string s = "{ uint64_t _b=c->x[" + std::to_string(rn) + "]; ";
            const char* addr = (mode == 1) ? "_b" : "(_b+_o)";
            s += "int64_t _o=" + std::to_string((long long)off) + "; ";
            if (rt != 31) {
                s += "c->x[" + std::to_string(rt) + "]=(uint64_t)(int64_t)(int32_t)(uint32_t)recomp_load32(c," +
                     addr + "); ";
            }
            if (rt2 != 31) {
                s += "c->x[" + std::to_string(rt2) + "]=(uint64_t)(int64_t)(int32_t)(uint32_t)recomp_load32(c," +
                     addr + "+4); ";
            }
            if (mode == 1 || mode == 3) {
                s += "c->x[" + std::to_string(rn) + "]=_b+_o; ";
            }
            s += "}";
            put(s);
            return true;
        }

        if ((opc == 0 || opc == 2) && mode >= 1 && mode <= 3) {
            const u32 sz = (opc == 2) ? 8 : 4;
            const s64 off = (s64)imm7 * sz;
            std::string s = "{ uint64_t _b=c->x[" + std::to_string(rn) + "]; ";
            // Pre-index and post-index both write the new base back; only
            // pre-index applies the offset before the access.
            const char* addr = (mode == 1) ? "_b" : "(_b+_o)";
            s += "int64_t _o=" + std::to_string((long long)off) + "; ";
            if (is_load) {
                // Rt/Rt2 == 31 is XZR here, so the loaded value is discarded -
                // writing it would land on c->x[31], which is where SP lives.
                if (rt != 31) {
                    s += "c->x[" + std::to_string(rt) + "]=recomp_load" + std::to_string(sz * 8) +
                         "(c," + addr + "); ";
                }
                if (rt2 != 31) {
                    s += "c->x[" + std::to_string(rt2) + "]=recomp_load" + std::to_string(sz * 8) +
                         "(c," + addr + "+" + std::to_string(sz) + "); ";
                }
            } else {
                s += "recomp_store" + std::to_string(sz * 8) + "(c," + addr + "," +
                     (rt == 31 ? std::string("(uint64_t)0") : ("c->x[" + std::to_string(rt) + "]")) + "); ";
                s += "recomp_store" + std::to_string(sz * 8) + "(c," + addr + "+" +
                     std::to_string(sz) + "," +
                     (rt2 == 31 ? std::string("(uint64_t)0") : ("c->x[" + std::to_string(rt2) + "]")) + "); ";
            }
            if (mode == 1 || mode == 3) {
                s += "c->x[" + std::to_string(rn) + "]=_b+_o; ";
            }
            s += "}";
            put(s);
            return true;
        }
    }

    // Logical immediate: AND/ORR/EOR/ANDS. By far the largest single gap in
    // real code - the immediate is a repeating bit pattern, not a literal.
    if ((i & 0x1F800000) == 0x12000000) {
        const u32 sf = i >> 31, opc = (i >> 29) & 3, N = (i >> 22) & 1;
        const u32 immr = (i >> 16) & 0x3F, imms = (i >> 10) & 0x3F;
        const u32 rn = (i >> 5) & 31, rd = i & 31;
        u64 imm = 0;
        if (DecodeBitMasks(N, imms, immr, sf != 0, imm)) {
            const char* op = (opc == 0 || opc == 3) ? "&" : (opc == 1 ? "|" : "^");
            std::string s = "{ uint64_t _r = " + Xz(rn) + " " + op + " 0x" ;
            char hb[32]; snprintf(hb, sizeof hb, "%llxULL", (unsigned long long)imm);
            s += hb; s += "; ";
            if (!sf) s += "_r &= 0xFFFFFFFFULL; ";
            // Rd==31 is SP for AND/ORR/EOR immediate and only reads as XZR
            // for ANDS (opc==3). Treating it as XZR everywhere silently
            // dropped every "and sp, xN, #imm" stack realignment.
            if (!(rd == 31 && opc == 3)) s += "c->x[" + std::to_string(rd) + "] = _r; ";
            if (opc == 3) s += "recomp_set_flags(c,0,_r,0,_r," + std::string(sf ? "1" : "0") + "); ";
            s += "}";
            put(s);
            return true;
        }
    }

    // EXTR: extract a register-width result from the concatenation Rn:Rm.
    if ((i & 0x7fa00000U) == 0x13800000U) {
        const u32 sf = i >> 31, width = sf ? 64 : 32;
        const u32 shift = (i >> 10) & 63;
        const u32 rn = (i >> 5) & 31, rm = (i >> 16) & 31, rd = i & 31;
        if (((i >> 22) & 1) == sf && shift < width) {
            if (rd != 31) {
                std::string s = "{ uint64_t _lo = " + Xz(rm) + ", _hi = " + Xz(rn) + "; ";
                if (!sf) s += "_lo &= 0xffffffffULL; _hi &= 0xffffffffULL; ";
                s += "c->x[" + std::to_string(rd) + "] = ";
                if (!shift) s += "_lo; }";
                else {
                    s += "((_lo >> " + std::to_string(shift) + ") | (_hi << " +
                         std::to_string(width - shift) + "))";
                    if (!sf) s += " & 0xffffffffULL";
                    s += "; }";
                }
                put(s);
            }
            return true;
        }
    }

    // Bitfield: UBFM/SBFM/BFM - the encoding behind LSL/LSR/ASR/UBFX/SBFX.
    if ((i & 0x1F800000) == 0x13000000) {
        const u32 sf = i >> 31, opc = (i >> 29) & 3;
        const u32 immr = (i >> 16) & 0x3F, imms = (i >> 10) & 0x3F;
        const u32 rn = (i >> 5) & 31, rd = i & 31;
        const u32 width = sf ? 64 : 32;

        // BFM (opc 1) - the encoding behind BFI and BFXIL. Unlike UBFM/SBFM it
        // merges into the destination, leaving the bits outside the inserted
        // field untouched, so it needs an explicit mask rather than a shift.
        if (rd != 31 && opc == 1 && immr < width && imms < width) {
            u32 nbits, src_shift, dst_pos;
            if (imms >= immr) {          // BFXIL: extract to the bottom of Rd
                nbits = imms - immr + 1;
                src_shift = immr;
                dst_pos = 0;
            } else {                     // BFI: insert at (width - immr)
                nbits = imms + 1;
                src_shift = 0;
                dst_pos = width - immr;
            }
            if (nbits + dst_pos <= width) {
                // Build the field mask at 64 bits; nbits is < 64 here because
                // dst_pos + nbits <= width <= 64 and a 64-wide field would
                // make the shift below undefined.
                const u64 field = (nbits >= 64) ? ~0ULL : ((1ULL << nbits) - 1ULL);
                const u64 mask = field << dst_pos;
                char mb[48], fb[48];
                snprintf(mb, sizeof mb, "0x%llxULL", (unsigned long long)mask);
                snprintf(fb, sizeof fb, "0x%llxULL", (unsigned long long)field);
                std::string s = "{ uint64_t _s = " + Xz(rn);
                if (src_shift) s += " >> " + std::to_string(src_shift);
                s += "; uint64_t _r = (c->x[" + std::to_string(rd) + "] & ~" + mb +
                     ") | ((_s & " + fb + ") << " + std::to_string(dst_pos) + "); ";
                if (!sf) s += "_r &= 0xFFFFFFFFULL; ";
                s += "c->x[" + std::to_string(rd) + "] = _r; }";
                put(s);
                return true;
            }
        }

        // UBFM (opc 2) and SBFM (opc 0).
        if (rd != 31 && (opc == 0 || opc == 2) && immr < width && imms < width) {
            const char* mask = sf ? "" : " & 0xFFFFFFFFULL";
            std::string src = Xz(rn);
            std::string s = "{ uint64_t _s = " + src + mask + "; uint64_t _r; ";
            if (imms >= immr) {
                // Extract (imms-immr+1) bits starting at immr.
                const u32 nbits = imms - immr + 1;
                s += "_r = (_s >> " + std::to_string(immr) + ")";
                if (nbits < 64) s += " & ((1ULL << " + std::to_string(nbits) + ") - 1)";
                s += "; ";
                if (opc == 0 && nbits < 64) { // SBFM: sign-extend from nbits
                    s += "if (_r & (1ULL << " + std::to_string(nbits - 1) + ")) _r |= ~((1ULL << " +
                         std::to_string(nbits) + ") - 1); ";
                }
            } else {
                // Insert: bits [0..imms] moved to start at (width-immr).
                const u32 nbits = imms + 1;
                const u32 shift = width - immr;
                s += "_r = (_s";
                if (nbits < 64) s += " & ((1ULL << " + std::to_string(nbits) + ") - 1)";
                s += ") << " + std::to_string(shift) + "; ";
                if (opc == 0 && shift + nbits < 64) {
                    s += "if (_r & (1ULL << " + std::to_string(shift + nbits - 1) +
                         ")) _r |= ~((1ULL << " + std::to_string(shift + nbits) + ") - 1); ";
                }
            }
            if (!sf) s += "_r &= 0xFFFFFFFFULL; ";
            s += "c->x[" + std::to_string(rd) + "] = _r; }";
            put(s);
            return true;
        }
    }

    // Conditional compare: CCMP/CCMN, both the register and 5-bit immediate
    // forms. When the condition holds this behaves as an ordinary compare;
    // when it does not, the flags are loaded straight from the nzcv field.
    // Short-circuit conditionals in C compile to chains of these.
    if ((i & 0x1FE00000) == 0x1A400000 && ((i >> 29) & 1) && ((i >> 10) & 1) == 0 &&
        ((i >> 4) & 1) == 0) {
        const u32 sf = i >> 31, op = (i >> 30) & 1;   // op: 0 = CCMN, 1 = CCMP
        const u32 imm_or_rm = (i >> 16) & 31, cond = (i >> 12) & 15;
        const bool is_imm = ((i >> 11) & 1) != 0;
        const u32 rn = (i >> 5) & 31, nzcv = i & 15;
        const std::string b = is_imm ? (std::to_string(imm_or_rm) + "ULL") : Xz(imm_or_rm);
        std::string s = "{ if (recomp_cond(c," + std::to_string(cond) + ")) { ";
        s += "uint64_t _a=" + Xz(rn) + ", _b=" + b + ", _r=" +
             std::string(op ? "_a-_b" : "_a+_b") + "; ";
        if (!sf) s += "_r &= 0xFFFFFFFFULL; ";
        s += "recomp_set_flags(c," + std::string(op ? "1" : "0") + ",_a,_b,_r," +
             (sf ? "1" : "0") + "); ";
        s += "} else { ";
        s += "c->n=" + std::to_string((nzcv >> 3) & 1) + "; ";
        s += "c->z=" + std::to_string((nzcv >> 2) & 1) + "; ";
        s += "c->c=" + std::to_string((nzcv >> 1) & 1) + "; ";
        s += "c->v=" + std::to_string(nzcv & 1) + "; } }";
        put(s);
        return true;
    }

    // Conditional select: CSEL/CSINC/CSINV/CSNEG.
    // Bit 11 must be clear: with it set this encoding is not a conditional
    // select at all, and decoding it as one silently invents an instruction.
    if ((i & 0x1FE00800) == 0x1A800000) {
        const u32 sf = i >> 31, op = (i >> 30) & 1, o2 = (i >> 10) & 1;
        const u32 rm = (i >> 16) & 31, cond = (i >> 12) & 15, rn = (i >> 5) & 31, rd = i & 31;
        if (rd != 31) {
            std::string a = Xz(rn), b = Xz(rm);
            std::string els = b;
            if (!op && o2) els = "(" + b + " + 1)";            // CSINC
            else if (op && !o2) els = "(~" + b + ")";           // CSINV
            else if (op && o2) els = "((uint64_t)(0 - " + b + "))"; // CSNEG
            std::string s = "{ uint64_t _r = recomp_cond(c," + std::to_string(cond) + ") ? " +
                            a + " : " + els + "; ";
            if (!sf) s += "_r &= 0xFFFFFFFFULL; ";
            s += "c->x[" + std::to_string(rd) + "] = _r; }";
            put(s);
            return true;
        }
    }

    // Load/store with unscaled 9-bit signed offset (LDUR/STUR) and the
    // pre/post-indexed immediate forms.
    // Again bit 26 (V) must be clear - the SIMD/FP form is handled below.
    // Bit 21 set with mode==0 is the ARMv8.1 LSE atomic group, not LDUR/STUR;
    // decoding those here would read a garbage imm9 out of the register field.
    if ((i & 0x3F000000) == 0x38000000 && ((i >> 24) & 1) == 0 && ((i >> 21) & 1) == 0) {
        const u32 size = i >> 30, opc = (i >> 22) & 3, mode = (i >> 10) & 3;
        const u32 rn = (i >> 5) & 31, rt = i & 31;
        s32 imm9 = (s32)((i >> 12) & 0x1FF);
        if (imm9 & 0x100) imm9 |= ~0x1FF;
        // mode 0 = LDUR/STUR, 1 = post-index, 3 = pre-index
        if ((mode == 0 || mode == 1 || mode == 3) && size <= 3) {
            const u32 bits = 8u << size;
            std::string s = "{ uint64_t _b=c->x[" + std::to_string(rn) + "]; int64_t _o=" +
                            std::to_string((long long)imm9) + "; ";
            const char* addr = (mode == 1) ? "_b" : "(_b+_o)";
            // opc selects the operation, and testing only its low bit gets this
            // wrong in both directions - the same mistake the unsigned-offset
            // handler already documents, never applied here. opc==2 (LDURSW /
            // LDURSB / LDURSH, sign-extending into a 64-bit destination) has
            // bit 0 clear and so was emitted as a STORE, turning tens of
            // thousands of ordinary signed loads into wild writes over live
            // guest memory; opc==3 loaded but never sign-extended.
            const char* signed_cast = size == 0   ? "(int8_t)"
                                      : size == 1 ? "(int16_t)"
                                                  : "(int32_t)";
            if (opc == 0) {
                s += "recomp_store" + std::to_string(bits) + "(c," + addr + "," + Xz(rt) + "); ";
            } else if (opc == 1) {
                if (rt != 31) {
                    s += "c->x[" + std::to_string(rt) + "]=recomp_load" + std::to_string(bits) +
                         "(c," + addr + "); ";
                }
            } else if (size == 3) {
                // opc>=2 with size==3 is PRFUM, a prefetch hint. It has no
                // architectural effect, so emitting nothing is exact - what it
                // must never do is store.
            } else if (rt != 31) {
                s += "c->x[" + std::to_string(rt) + "]=(uint64_t)(int64_t)" + signed_cast +
                     "recomp_load" + std::to_string(bits) + "(c," + addr + "); ";
                // opc==3 sign-extends into a 32-bit destination, so the result
                // is truncated back to W width after the extension.
                if (opc == 3) s += "c->x[" + std::to_string(rt) + "]&=0xFFFFFFFFULL; ";
            }
            if (mode == 1 || mode == 3) s += "c->x[" + std::to_string(rn) + "]=_b+_o; ";
            s += "}";
            put(s);
            return true;
        }
    }

    // Exclusive operations use the same global monitor as the JIT and kernel.
    if ((i & 0x3F000000) == 0x08000000) {
        const u32 size = i >> 30, o2 = (i >> 23) & 1, L = (i >> 22) & 1, o1 = (i >> 21) & 1;
        const u32 rt2 = (i >> 10) & 31, rn = (i >> 5) & 31, rt = i & 31;
        if (!o2 && ((!o1 && rt2 == 31) || (o1 && size >= 2))) {
            snprintf(buf, sizeof buf,
                     "recomp_exclusive(c,0x%08xU,g_module_base+0x%llxULL); if(c->halted) return;",
                     i, (unsigned long long)pc);
            put(buf);
            return true;
        }
        if (o2 && !o1 && rt2 == 31) {
            const u32 bits = 8u << size;
            const std::string addr = "c->x[" + std::to_string(rn) + "]";
            if (L) {
                // LDAR
                if (rt != 31) {
                    put("c->x[" + std::to_string(rt) + "] = recomp_load" +
                        std::to_string(bits) + "(c," + addr + ");");
                }
            } else {
                // STLR - no status register.
                put("recomp_store" + std::to_string(bits) + "(c," + addr + "," + Xz(rt) + ");");
            }
            return true;
        }
    }

    // Data-processing 3-source: MADD/MSUB and the widening multiplies.
    if ((i & 0x1F000000) == 0x1B000000) {
        const u32 sf = i >> 31, op54 = (i >> 29) & 3, op31 = (i >> 21) & 7, o0 = (i >> 15) & 1;
        const u32 rm = (i >> 16) & 31, ra = (i >> 10) & 31, rn = (i >> 5) & 31, rd = i & 31;
        if (rd != 31 && op54 == 0) {
            std::string expr;
            if (op31 == 0) {
                // MADD / MSUB
                const std::string prod = "(" + Xz(rn) + " * " + Xz(rm) + ")";
                expr = Xz(ra) + (o0 ? " - " : " + ") + prod;
            } else if (op31 == 1 && !o0) {           // SMADDL
                expr = Xz(ra) + " + (uint64_t)((int64_t)(int32_t)" + Xz(rn) +
                       " * (int64_t)(int32_t)" + Xz(rm) + ")";
            } else if (op31 == 1 && o0) {            // SMSUBL
                expr = Xz(ra) + " - (uint64_t)((int64_t)(int32_t)" + Xz(rn) +
                       " * (int64_t)(int32_t)" + Xz(rm) + ")";
            } else if (op31 == 5 && !o0) {           // UMADDL
                expr = Xz(ra) + " + ((uint64_t)(uint32_t)" + Xz(rn) +
                       " * (uint64_t)(uint32_t)" + Xz(rm) + ")";
            } else if (op31 == 5 && o0) {            // UMSUBL
                expr = Xz(ra) + " - ((uint64_t)(uint32_t)" + Xz(rn) +
                       " * (uint64_t)(uint32_t)" + Xz(rm) + ")";
            } else if (op31 == 2) {                  // SMULH
                expr = "recomp_smulh(" + Xz(rn) + "," + Xz(rm) + ")";
            } else if (op31 == 6) {                  // UMULH
                expr = "recomp_umulh(" + Xz(rn) + "," + Xz(rm) + ")";
            }
            if (!expr.empty()) {
                std::string s = "{ uint64_t _r = " + expr + "; ";
                if (!sf && op31 == 0) s += "_r &= 0xFFFFFFFFULL; ";
                s += "c->x[" + std::to_string(rd) + "] = _r; }";
                put(s);
                return true;
            }
        }
    }

    // ADD/SUB extended register - the form used for pointer arithmetic with a
    // 32-bit index, so extremely common around array and struct accesses.
    if ((i & 0x1F200000) == 0x0B200000) {
        const u32 sf = i >> 31, op = (i >> 30) & 1, S = (i >> 29) & 1;
        const u32 rm = (i >> 16) & 31, option = (i >> 13) & 7, imm3 = (i >> 10) & 7;
        const u32 rn = (i >> 5) & 31, rd = i & 31;
        if (imm3 <= 4) {
            std::string ext;
            switch (option) {
            case 0: ext = "(uint64_t)(uint8_t)" + Xz(rm); break;           // UXTB
            case 1: ext = "(uint64_t)(uint16_t)" + Xz(rm); break;          // UXTH
            case 2: ext = "(uint64_t)(uint32_t)" + Xz(rm); break;          // UXTW
            case 3: ext = Xz(rm); break;                                    // UXTX/LSL
            case 4: ext = "(uint64_t)(int64_t)(int8_t)" + Xz(rm); break;   // SXTB
            case 5: ext = "(uint64_t)(int64_t)(int16_t)" + Xz(rm); break;  // SXTH
            case 6: ext = "(uint64_t)(int64_t)(int32_t)" + Xz(rm); break;  // SXTW
            case 7: ext = Xz(rm); break;                                    // SXTX
            default: break;
            }
            if (!ext.empty()) {
                if (imm3) ext = "((" + ext + ") << " + std::to_string(imm3) + ")";
                // Rn is SP here, not the zero register, and so is Rd unless the
                // flag-setting form is used - where it really is the zero
                // register, because that is CMP.
                std::string s = "{ uint64_t _a=" + Xsp(rn) + ", _b=" + ext + "; uint64_t _r=" +
                                std::string(op ? "_a-_b" : "_a+_b") + "; ";
                if (!sf) s += "_r &= 0xFFFFFFFFULL; ";
                if (rd != 31 || !S) s += "c->x[" + std::to_string(rd) + "]=_r; ";
                if (S) s += "recomp_set_flags(c," + std::string(op ? "1" : "0") +
                            ",_a,_b,_r," + (sf ? "1" : "0") + "); ";
                s += "}";
                put(s);
                return true;
            }
        }
    }

    // Load/store with a register offset (the [base, Xm{, extend}] form).
    // Bit 26 (V) clear: general registers only, SIMD/FP form handled below.
    if ((i & 0x3F200C00) == 0x38200800) {
        const u32 size = i >> 30, opc = (i >> 22) & 3;
        const u32 rm = (i >> 16) & 31, option = (i >> 13) & 7, S = (i >> 12) & 1;
        const u32 rn = (i >> 5) & 31, rt = i & 31;
        // opc 00 stores, 01 loads zero-extended, 10 loads sign-extended to 64
        // bits and 11 sign-extended to 32. The signed forms are common - an
        // indexed read of an int32 array compiles to LDRSW with a register
        // offset - so leaving them out put thousands of real loads on the
        // fallback.
        if (size <= 3 && !(opc >= 2 && size == 3)) {
            const u32 bits = 8u << size;
            std::string idx;
            switch (option) {
            case 2: idx = "(uint64_t)(uint32_t)" + Xz(rm); break;          // UXTW
            case 3: idx = Xz(rm); break;                                    // LSL
            case 6: idx = "(uint64_t)(int64_t)(int32_t)" + Xz(rm); break;  // SXTW
            case 7: idx = Xz(rm); break;                                    // SXTX
            default: break;
            }
            if (!idx.empty()) {
                if (S && size) idx = "((" + idx + ") << " + std::to_string(size) + ")";
                const std::string addr = "(c->x[" + std::to_string(rn) + "] + " + idx + ")";
                if (opc == 0) {
                    put("recomp_store" + std::to_string(bits) + "(c," + addr + "," + Xz(rt) + ");");
                } else if (opc == 1) {
                    if (rt != 31) {
                        put("c->x[" + std::to_string(rt) + "] = recomp_load" +
                            std::to_string(bits) + "(c," + addr + ");");
                    }
                } else if (rt != 31) {
                    const char* st = size == 0 ? "int8_t" : size == 1 ? "int16_t" : "int32_t";
                    std::string s = "{ uint64_t _r = (uint64_t)(int64_t)(" + std::string(st) +
                                    ")recomp_load" + std::to_string(bits) + "(c," + addr + "); ";
                    if (opc == 3) s += "_r &= 0xFFFFFFFFULL; ";   // 32-bit destination
                    s += "c->x[" + std::to_string(rt) + "] = _r; }";
                    put(s);
                }
                return true;
            }
        }
    }

    // FP <-> integer conversions and FMOV between register files. These share
    // bit 21 with the FP arithmetic forms and are distinguished by bits 15..10
    // being zero, so they must be decoded ahead of the arithmetic/compare block
    // below or every one of them is mis-decoded as FCMP.
    if ((i & 0x5F200000) == 0x1E200000 && ((i >> 21) & 1) && ((i >> 10) & 0x3F) == 0) {
        const u32 sf = i >> 31, ftype = (i >> 22) & 3;
        const u32 rmode = (i >> 19) & 3, opcode = (i >> 16) & 7;
        const u32 rn = (i >> 5) & 31, rd = i & 31;
        if (ftype == 0 || ftype == 1) {
            const bool dbl = (ftype == 1);
            const char* ct = dbl ? "double" : "float";
            const int fsz = dbl ? 8 : 4;
            const std::string zero_d = "c->vreg[" + std::to_string(rd) + "][0]=0; c->vreg[" +
                                       std::to_string(rd) + "][1]=0; ";

            // SCVTF / UCVTF: integer register -> FP register.
            if (rmode == 0 && (opcode == 2 || opcode == 3)) {
                const std::string src = sf ? (opcode == 2 ? "(int64_t)" + Xz(rn)
                                                          : "(uint64_t)" + Xz(rn))
                                           : (opcode == 2 ? "(int32_t)" + Xz(rn)
                                                          : "(uint32_t)" + Xz(rn));
                put("{ " + std::string(ct) + " _r = (" + ct + ")(" + src + "); " + zero_d +
                    "memcpy(&c->vreg[" + std::to_string(rd) + "][0],&_r," +
                    std::to_string(fsz) + "); }");
                return true;
            }

            // FP register -> integer register. The rounding mode is part of
            // the instruction: rmode picks nearest-even, +inf, -inf or zero,
            // and the separate FCVTA* opcodes round half away from zero. The
            // value is rounded first and only then converted with saturation,
            // which is the order the architecture specifies.
            const bool cvt_rounded = (opcode == 0 || opcode == 1);
            const bool cvt_away = (rmode == 0 && (opcode == 4 || opcode == 5));
            if ((cvt_rounded || cvt_away) && rd != 31) {
                const bool is_signed = cvt_away ? (opcode == 4) : (opcode == 0);
                const char* it = sf ? (is_signed ? "int64_t" : "uint64_t")
                                    : (is_signed ? "int32_t" : "uint32_t");
                // FCVTN* nearest-even, FCVTP* toward +inf, FCVTM* toward -inf,
                // FCVTZ* toward zero, FCVTA* nearest with ties away from zero.
                const char* round_fn = nullptr;
                if (cvt_away)          round_fn = dbl ? "round" : "roundf";
                else if (rmode == 0)   round_fn = dbl ? "rint"  : "rintf";
                else if (rmode == 1)   round_fn = dbl ? "ceil"  : "ceilf";
                else if (rmode == 2)   round_fn = dbl ? "floor" : "floorf";
                else                   round_fn = dbl ? "trunc" : "truncf";
                std::string s = "{ " + std::string(ct) + " _a; memcpy(&_a,&c->vreg[" +
                                std::to_string(rn) + "][0]," + std::to_string(fsz) + "); ";
                s += "_a = " + std::string(round_fn) + "(_a); ";
                // All of these saturate: NaN gives 0, and anything outside the
                // destination's range clamps to that range's min or max. A bare
                // C cast is undefined for exactly those inputs, and on x86 it
                // compiles to cvttss2si, which answers "integer indefinite"
                // (INT_MIN / LLONG_MIN) for NaN, +inf and -inf alike. Float-heavy
                // game code converts out-of-range values constantly - clamped
                // indices, hashes, fixed-point - so the wrong answer here is a
                // steady drip of corruption rather than an immediate fault.
                const char* lo_bound = sf ? (is_signed ? "-9223372036854775808.0" : "0.0")
                                          : (is_signed ? "-2147483648.0" : "0.0");
                const char* hi_bound = sf ? (is_signed ? "9223372036854775807.0"
                                                       : "18446744073709551615.0")
                                          : (is_signed ? "2147483647.0" : "4294967295.0");
                const char* sat_lo = sf ? (is_signed ? "0x8000000000000000ULL" : "0ULL")
                                        : (is_signed ? "0xFFFFFFFF80000000ULL" : "0ULL");
                const char* sat_hi = sf ? (is_signed ? "0x7FFFFFFFFFFFFFFFULL"
                                                     : "0xFFFFFFFFFFFFFFFFULL")
                                        : (is_signed ? "0x7FFFFFFFULL" : "0xFFFFFFFFULL");
                s += "uint64_t _r; if (_a != _a) _r = 0ULL; ";
                s += std::string("else if (!(_a > (") + ct + ")" + lo_bound + ")) _r = " + sat_lo + "; ";
                s += std::string("else if (!(_a < (") + ct + ")" + hi_bound + ")) _r = " + sat_hi + "; ";
                s += "else _r = (uint64_t)(" + std::string(it) + ")_a; ";
                if (!sf) s += "_r &= 0xFFFFFFFFULL; ";
                s += "c->x[" + std::to_string(rd) + "] = _r; }";
                put(s);
                return true;
            }

            // FMOV between a general register and an FP register.
            if (rmode == 0 && (opcode == 6 || opcode == 7)) {
                if (opcode == 7) {           // GPR -> FP
                    put("{ " + zero_d + "memcpy(&c->vreg[" + std::to_string(rd) + "][0],&" +
                        (rn == 31 ? std::string("(uint64_t){0}") : "c->x[" + std::to_string(rn) + "]") +
                        "," + std::to_string(fsz) + "); }");
                    return true;
                }
                if (rd != 31) {              // FP -> GPR
                    put("{ uint64_t _r=0; memcpy(&_r,&c->vreg[" + std::to_string(rn) + "][0]," +
                        std::to_string(fsz) + "); c->x[" + std::to_string(rd) + "]=_r; }");
                    return true;
                }
            }
        }
    }

    // FCSEL - the floating-point conditional select. Shares the scalar FP
    // encoding but is picked out by bits 11..10 being 11, so it is matched
    // before the arithmetic and compare forms below.
    if ((i & 0x5F200C00) == 0x1E200C00) {
        const u32 ftype = (i >> 22) & 3;
        const u32 rm = (i >> 16) & 31, cond = (i >> 12) & 15;
        const u32 rn = (i >> 5) & 31, rd = i & 31;
        if (ftype == 0 || ftype == 1) {
            const int fsz = (ftype == 1) ? 8 : 4;
            // Selecting whole register halves rather than reinterpreting the
            // value keeps this exact for NaN payloads too.
            put("{ uint64_t _r = recomp_cond(c," + std::to_string(cond) + ") ? c->vreg[" +
                std::to_string(rn) + "][0] : c->vreg[" + std::to_string(rm) + "][0]; " +
                (fsz == 4 ? "_r &= 0xFFFFFFFFULL; " : "") + "c->vreg[" + std::to_string(rd) +
                "][0]=_r; c->vreg[" + std::to_string(rd) + "][1]=0; }");
            return true;
        }
    }

    // Data-processing (1 source): RBIT, REV16, REV32, REV, CLZ and CLS.
    // The mask must stop at bit 16: bits 15..10 are the opcode being switched
    // on below, so including them would pin this to RBIT alone.
    if ((i & 0x7FFF0000) == 0x5AC00000) {
        const u32 sf = i >> 31, opcode = (i >> 10) & 0x3F;
        const u32 rn = (i >> 5) & 31, rd = i & 31;
        const u32 width = sf ? 64 : 32;
        if (rd != 31) {
            const std::string w = std::to_string(width);
            const std::string src =
                sf ? Xz(rn) : ("(" + Xz(rn) + " & 0xFFFFFFFFULL)");
            std::string s;
            switch (opcode) {
            case 0:   // RBIT - reverse every bit
                s = "{ uint64_t _v=" + src + ", _r=0; for(int _i=0;_i<" + w +
                    ";_i++){ _r=(_r<<1)|((_v>>_i)&1ULL); } c->x[" + std::to_string(rd) + "]=_r; }";
                break;
            case 1:   // REV16 - reverse bytes within each halfword
                s = "{ uint64_t _v=" + src + ", _r=0; for(int _i=0;_i<" + w +
                    "/8;_i++){ int _p=(_i^1)*8; _r|=((_v>>(_i*8))&0xFFULL)<<_p; } c->x[" +
                    std::to_string(rd) + "]=_r; }";
                break;
            case 2:   // REV32 on 64-bit, REV on 32-bit: bytes within each word
                if (!sf) {
                    s = "{ uint64_t _v=" + src + ", _r=0; for(int _i=0;_i<4;_i++){ "
                        "_r|=((_v>>(_i*8))&0xFFULL)<<((3-_i)*8); } c->x[" +
                        std::to_string(rd) + "]=_r; }";
                } else {
                    s = "{ uint64_t _v=" + src + ", _r=0; for(int _i=0;_i<8;_i++){ "
                        "int _p=((_i&4)|(3-(_i&3)))*8; _r|=((_v>>(_i*8))&0xFFULL)<<_p; } c->x[" +
                        std::to_string(rd) + "]=_r; }";
                }
                break;
            case 3:   // REV (64-bit only) - reverse all bytes
                if (sf) {
                    s = "{ uint64_t _v=" + src + ", _r=0; for(int _i=0;_i<8;_i++){ "
                        "_r|=((_v>>(_i*8))&0xFFULL)<<((7-_i)*8); } c->x[" +
                        std::to_string(rd) + "]=_r; }";
                }
                break;
            case 4:   // CLZ
                s = "{ uint64_t _v=" + src + "; int _n=0; while(_n<" + w + " && !((_v>>(" + w +
                    "-1-_n))&1ULL)) _n++; c->x[" + std::to_string(rd) + "]=(uint64_t)_n; }";
                break;
            case 5:   // CLS - leading sign bits, not counting the sign itself
                s = "{ uint64_t _v=" + src + "; int _n=0; while(_n<" + w +
                    "-1 && (((_v>>(" + w + "-1-_n))&1ULL)==((_v>>(" + w +
                    "-2-_n))&1ULL))) _n++; c->x[" + std::to_string(rd) + "]=(uint64_t)_n; }";
                break;
            default: break;
            }
            if (!s.empty()) { put(s); return true; }
        }
    }

    // Cache maintenance: the DC and IC forms of SYS. Every one of these is a
    // no-op for this backend. The host view of memory is coherent, and
    // Core::ArmRecomp deliberately does not support self-modifying guest code
    // (ClearInstructionCache and InvalidateCacheRange are empty for the same
    // reason), so there is no state for a clean or invalidate to act on.
    // DC ZVA is excluded: it zeroes a block of memory and is a real store, not
    // a hint, so it stays on the fallback rather than being silently dropped.
    if ((i & 0xFFF80000) == 0xD5080000) {
        const u32 op1 = (i >> 16) & 7, crn = (i >> 12) & 15, crm = (i >> 8) & 15;
        const u32 rt = i & 31;
        const bool maintenance = crn == 7 &&
                                 (crm == 5 || crm == 6 || crm == 10 || crm == 11 || crm == 14);
        if (maintenance && (op1 == 0 || op1 == 3)) {
            put("(void)" + Xz(rt) + ";");
            return true;
        }
    }

    // MRS/MSR against the small set of system registers a user-mode program
    // actually touches. Anything else stays on the fallback rather than being
    // silently invented.
    if ((i & 0xFFF00000) == 0xD5300000 || (i & 0xFFF00000) == 0xD5100000) {
        const bool is_read = ((i >> 21) & 1) != 0;   // MRS reads, MSR writes
        const u32 sysreg = (i >> 5) & 0x7FFF;
        const u32 rt = i & 31;
        // TPIDR_EL0 (thread pointer) and TPIDRRO_EL0 are the ones that matter
        // for ordinary code; both live in the context already. They are kept
        // in *separate* fields deliberately: TPIDR_EL0 is the guest's own
        // thread pointer, while TPIDRRO_EL0 is written by the kernel and holds
        // the thread-local region whose first bytes are the IPC message
        // buffer. Folding them together corrupts both.
        constexpr u32 kTpidrEl0 = 0x5E82;
        constexpr u32 kTpidrroEl0 = 0x5E83;
        // CTR_EL0, the cache type register. Read-only, and read by every
        // hand-written memcpy/cache-maintenance routine in the SDK to find the
        // cache line size. The value must be the one the JIT reports
        // (Core::ArmDynarmic64 sets config.ctr_el0), or the same guest code
        // takes different paths depending on which backend ran it.
        constexpr u32 kCtrEl0 = 0x5801;
        if (sysreg == kCtrEl0) {
            if (is_read) {
                if (rt != 31) put("c->x[" + std::to_string(rt) + "] = 0x8444c004ULL;");
            } else {
                put("(void)" + Xz(rt) + ";");
            }
            return true;
        }
        if (sysreg == kTpidrEl0) {
            if (is_read) {
                if (rt != 31) put("c->x[" + std::to_string(rt) + "] = c->tpidr_el0;");
            } else {
                put("c->tpidr_el0 = " + Xz(rt) + ";");
            }
            return true;
        }
        // Os registos de contagem - CNTFRQ_EL0, CNTPCT_EL0 e CNTVCT_EL0 - nao
        // podem ser respondidos aqui. O valor tem de vir da mesma contagem que
        // o JIT usa (CoreTiming), ou o mesmo codigo de convidado mede tempos
        // diferentes conforme o backend que executou o bloco. Vao pelo gancho
        // nativo que as operacoes exclusivas ja usam, que o emulador serve a
        // partir de CoreTiming::GetClockTicks e de Hardware::CNTFREQ.
        constexpr u32 kCntfrqEl0 = 0x5F00;
        constexpr u32 kCntpctEl0 = 0x5F01;
        constexpr u32 kCntvctEl0 = 0x5F02;
        if (is_read && (sysreg == kCntfrqEl0 || sysreg == kCntpctEl0 || sysreg == kCntvctEl0)) {
            snprintf(buf, sizeof buf,
                     "recomp_exclusive(c,0x%08xU,g_module_base+0x%llxULL); if(c->halted) return;",
                     i, (unsigned long long)pc);
            put(buf);
            return true;
        }
        // NZCV. Os quatro bits ja vivem no contexto, por isso a leitura e a
        // escrita sao exatas; nada aqui depende de estado que o codigo gerado
        // nao modele.
        constexpr u32 kNzcv = 0x5A10;
        if (sysreg == kNzcv) {
            if (is_read) {
                if (rt != 31)
                    put("c->x[" + std::to_string(rt) + "] = ((uint64_t)c->n << 31) | "
                        "((uint64_t)c->z << 30) | ((uint64_t)c->c << 29) | "
                        "((uint64_t)c->v << 28);");
            } else {
                put("{ uint64_t _f = " + Xz(rt) + "; c->n=(uint8_t)((_f >> 31) & 1); "
                    "c->z=(uint8_t)((_f >> 30) & 1); c->c=(uint8_t)((_f >> 29) & 1); "
                    "c->v=(uint8_t)((_f >> 28) & 1); }");
            }
            return true;
        }
        // FPCR e FPSR ficam deliberadamente de fora. O codigo gerado calcula
        // sempre em nearest-even sem flush-to-zero e nunca le estes registos,
        // por isso guardar o valor escrito daria uma leitura coerente por cima
        // de um modo que nao e respeitado - a mesma armadilha de decodificar
        // FRECPE como SCVTF. No fallback a escrita chega ao JIT, que a honra.
        if (sysreg == kTpidrroEl0) {
            if (is_read) {
                if (rt != 31) put("c->x[" + std::to_string(rt) + "] = c->tpidrro_el0;");
            } else {
                // Read-only at EL0; a write traps on hardware. Swallow it
                // rather than letting it destroy the kernel's TLS pointer.
                put("(void)" + Xz(rt) + ";");
            }
            return true;
        }
    }

    // Scalar floating-point three-source: FMADD, FMSUB, FNMADD and FNMSUB.
    // These are the fused forms - the product is not rounded before the add -
    // so they go through fma()/fmaf() rather than a written-out "a*b+c", which
    // would round twice and drift from the hardware.
    if ((i & 0x5F000000) == 0x1F000000) {
        const u32 ftype = (i >> 22) & 3, o1 = (i >> 21) & 1, o0 = (i >> 15) & 1;
        const u32 rm = (i >> 16) & 31, ra = (i >> 10) & 31, rn = (i >> 5) & 31, rd = i & 31;
        if (ftype == 0 || ftype == 1) {
            const bool dbl = (ftype == 1);
            const char* ct = dbl ? "double" : "float";
            const int sz = dbl ? 8 : 4;
            const char* fma = dbl ? "fma" : "fmaf";
            // FMADD a+n*m, FMSUB a-n*m, FNMADD -a-n*m, FNMSUB -a+n*m. o1
            // negates the accumulator; the product is negated when o0 and o1
            // differ, which is why FNMSUB - the o1=1, o0=1 encoding - keeps a
            // positive product and only FNMADD negates both.
            const std::string np = (o0 ^ o1) ? "-_n" : "_n";
            const std::string na = o1 ? "-_c" : "_c";
            std::string s = "{ " + std::string(ct) + " _n,_m,_c,_r; ";
            s += "memcpy(&_n,&c->vreg[" + std::to_string(rn) + "][0]," + std::to_string(sz) + "); ";
            s += "memcpy(&_m,&c->vreg[" + std::to_string(rm) + "][0]," + std::to_string(sz) + "); ";
            s += "memcpy(&_c,&c->vreg[" + std::to_string(ra) + "][0]," + std::to_string(sz) + "); ";
            s += "_r = " + std::string(fma) + "(" + np + ",_m," + na + "); ";
            s += "c->vreg[" + std::to_string(rd) + "][0]=0; c->vreg[" + std::to_string(rd) +
                 "][1]=0; memcpy(&c->vreg[" + std::to_string(rd) + "][0],&_r," +
                 std::to_string(sz) + "); }";
            put(s);
            return true;
        }
    }

    // Conversion between floating point and fixed point. Same shape as the
    // integer conversions above but with bit 21 clear and a scale field: the
    // integer operand carries `fbits` fraction bits, so it is the plain
    // integer divided by 2^fbits.
    if ((i & 0x5F200000) == 0x1E000000 && ((i >> 21) & 1) == 0) {
        const u32 sf = i >> 31, ftype = (i >> 22) & 3;
        const u32 rmode = (i >> 19) & 3, opcode = (i >> 16) & 7;
        const u32 scale = (i >> 10) & 0x3F;
        const u32 rn = (i >> 5) & 31, rd = i & 31;
        const u32 fbits = 64 - scale;
        if ((ftype == 0 || ftype == 1) && fbits >= 1 && fbits <= (sf ? 64u : 32u)) {
            const bool dbl = (ftype == 1);
            const char* ct = dbl ? "double" : "float";
            const int fsz = dbl ? 8 : 4;
            // 2^fbits as an exact constant; the widths involved are all within
            // the range a double represents exactly, so the scaling itself
            // introduces no error of its own.
            char scale_lit[64];
            snprintf(scale_lit, sizeof scale_lit, "%s0x1p%u", dbl ? "" : "(float)", fbits);

            if (rmode == 0 && (opcode == 2 || opcode == 3)) {   // SCVTF / UCVTF
                const std::string src = sf ? (opcode == 2 ? "(int64_t)" + Xz(rn)
                                                          : "(uint64_t)" + Xz(rn))
                                           : (opcode == 2 ? "(int32_t)" + Xz(rn)
                                                          : "(uint32_t)" + Xz(rn));
                put("{ " + std::string(ct) + " _r = (" + ct + ")(" + src + ") / " + scale_lit +
                    "; c->vreg[" + std::to_string(rd) + "][0]=0; c->vreg[" + std::to_string(rd) +
                    "][1]=0; memcpy(&c->vreg[" + std::to_string(rd) + "][0],&_r," +
                    std::to_string(fsz) + "); }");
                return true;
            }
            if (rmode == 3 && (opcode == 0 || opcode == 1) && rd != 31) {  // FCVTZS / FCVTZU
                const bool is_signed = (opcode == 0);
                const char* it = sf ? (is_signed ? "int64_t" : "uint64_t")
                                    : (is_signed ? "int32_t" : "uint32_t");
                const char* lo_bound = sf ? (is_signed ? "-9223372036854775808.0" : "0.0")
                                          : (is_signed ? "-2147483648.0" : "0.0");
                const char* hi_bound = sf ? (is_signed ? "9223372036854775807.0"
                                                       : "18446744073709551615.0")
                                          : (is_signed ? "2147483647.0" : "4294967295.0");
                const char* sat_lo = sf ? (is_signed ? "0x8000000000000000ULL" : "0ULL")
                                        : (is_signed ? "0xFFFFFFFF80000000ULL" : "0ULL");
                const char* sat_hi = sf ? (is_signed ? "0x7FFFFFFFFFFFFFFFULL"
                                                     : "0xFFFFFFFFFFFFFFFFULL")
                                        : (is_signed ? "0x7FFFFFFFULL" : "0xFFFFFFFFULL");
                std::string s = "{ " + std::string(ct) + " _a; memcpy(&_a,&c->vreg[" +
                                std::to_string(rn) + "][0]," + std::to_string(fsz) + "); ";
                s += "_a = _a * " + std::string(scale_lit) + "; ";
                // Same saturating conversion as the integer form: NaN gives 0
                // and out-of-range clamps, which a bare cast would not do.
                s += "uint64_t _r; if (_a != _a) _r = 0ULL; ";
                s += std::string("else if (!(_a > (") + ct + ")" + lo_bound + ")) _r = " + sat_lo + "; ";
                s += std::string("else if (!(_a < (") + ct + ")" + hi_bound + ")) _r = " + sat_hi + "; ";
                s += "else _r = (uint64_t)(" + std::string(it) + ")_a; ";
                if (!sf) s += "_r &= 0xFFFFFFFFULL; ";
                s += "c->x[" + std::to_string(rd) + "] = _r; }";
                put(s);
                return true;
            }
        }
    }

    // Scalar floating point. Only single (ftype 00) and double (ftype 01) are
    // handled; half-precision and the SIMD vector forms still fall through.
    // Values move through memcpy rather than type punning so this stays
    // strictly conforming C.
    if ((i & 0x5F000000) == 0x1E000000 && ((i >> 21) & 1)) {
        const u32 ftype = (i >> 22) & 3;
        const u32 rm = (i >> 16) & 31, rn = (i >> 5) & 31, rd = i & 31;
        if (ftype == 0 || ftype == 1) {
            const bool dbl = (ftype == 1);
            const char* ct = dbl ? "double" : "float";
            const int sz = dbl ? 8 : 4;
            const std::string ld_n = std::string("{ ") + ct + " _a,_b,_r; memcpy(&_a,&c->vreg[" +
                                     std::to_string(rn) + "][0]," + std::to_string(sz) + "); ";
            const std::string ld_m = std::string("memcpy(&_b,&c->vreg[") + std::to_string(rm) +
                                     "][0]," + std::to_string(sz) + "); ";
            const std::string st_d = std::string("c->vreg[") + std::to_string(rd) +
                                     "][0]=0; c->vreg[" + std::to_string(rd) +
                                     "][1]=0; memcpy(&c->vreg[" + std::to_string(rd) + "][0],&_r," +
                                     std::to_string(sz) + "); }";

            // FMOV (immediate). This must be decoded before FCMP below: it
            // also has bits 11..10 clear, so an FMOV whose imm8 and Rd happen
            // to line up would otherwise be read as a compare and silently
            // clobber the flags instead of loading a constant.
            if (((i >> 10) & 7) == 4 && ((i >> 5) & 0x1F) == 0) {
                const u32 imm8 = (i >> 13) & 0xFF;
                // VFPExpandImm: sign, then exponent as NOT(b6) followed by b6
                // repeated, then imm8<5:4>, then imm8<3:0> as the top of the
                // mantissa. Built at double width and narrowed if needed.
                const u32 sgn = (imm8 >> 7) & 1;
                const u32 b6 = (imm8 >> 6) & 1;
                const u64 e11 = ((u64)(b6 ^ 1) << 10) | (b6 ? (0xFFULL << 2) : 0ULL) |
                                ((imm8 >> 4) & 3);
                const u64 dbits = ((u64)sgn << 63) | (e11 << 52) | ((u64)(imm8 & 0xF) << 48);
                char hb[64];
                if (dbl) {
                    snprintf(hb, sizeof hb, "0x%llxULL", (unsigned long long)dbits);
                } else {
                    double dv; memcpy(&dv, &dbits, 8);
                    const float fv = (float)dv;
                    u32 fbits; memcpy(&fbits, &fv, 4);
                    snprintf(hb, sizeof hb, "0x%xULL", fbits);
                }
                put("c->vreg[" + std::to_string(rd) + "][0]=" + hb + "; c->vreg[" +
                    std::to_string(rd) + "][1]=0;");
                return true;
            }

            // Two-source: FMUL/FDIV/FADD/FSUB (opcode in bits 15..12).
            if (((i >> 10) & 3) == 2) {
                const u32 opcode = (i >> 12) & 15;
                const char* op = nullptr;
                switch (opcode) {
                case 0: op = "*"; break;
                case 1: op = "/"; break;
                case 2: op = "+"; break;
                case 3: op = "-"; break;
                default: break;
                }
                if (op) {
                    put(ld_n + ld_m + "_r = _a " + op + " _b; " + st_d);
                    return true;
                }
                // FMAX/FMIN propagate a NaN operand; the NM forms return the
                // other operand instead, which is exactly what fmax/fmin do.
                std::string expr2;
                switch (opcode) {
                case 4: expr2 = "(_a!=_a||_b!=_b) ? (_a+_b) : (_a>_b?_a:_b)"; break;  // FMAX
                case 5: expr2 = "(_a!=_a||_b!=_b) ? (_a+_b) : (_a<_b?_a:_b)"; break;  // FMIN
                case 6: expr2 = dbl ? "fmax(_a,_b)" : "fmaxf(_a,_b)"; break;          // FMAXNM
                case 7: expr2 = dbl ? "fmin(_a,_b)" : "fminf(_a,_b)"; break;          // FMINNM
                case 8: expr2 = "-(_a*_b)"; break;                                    // FNMUL
                default: break;
                }
                if (!expr2.empty()) {
                    put(ld_n + ld_m + "_r = " + expr2 + "; " + st_d);
                    return true;
                }
            }

            // FCVT between precisions. It shares the one-source encoding but
            // its source and destination widths differ, so it cannot use the
            // shared load/store fragments below, which assume both are ftype.
            // Opcode 0001xx, where the low two bits name the destination:
            // 00 single, 01 double, 11 half. Half stays on the fallback.
            if (((i >> 10) & 0x1F) == 0x10 && (((i >> 15) & 0x3C) == 0x04)) {
                const u32 dst = (i >> 15) & 3;
                if (ftype == 0 && dst == 1) {           // single -> double
                    put("{ float _s; double _d; memcpy(&_s,&c->vreg[" + std::to_string(rn) +
                        "][0],4); _d=(double)_s; c->vreg[" + std::to_string(rd) +
                        "][1]=0; memcpy(&c->vreg[" + std::to_string(rd) + "][0],&_d,8); }");
                    return true;
                }
                if (ftype == 1 && dst == 0) {           // double -> single
                    put("{ double _d; float _s; memcpy(&_d,&c->vreg[" + std::to_string(rn) +
                        "][0],8); _s=(float)_d; c->vreg[" + std::to_string(rd) +
                        "][0]=0; c->vreg[" + std::to_string(rd) +
                        "][1]=0; memcpy(&c->vreg[" + std::to_string(rd) + "][0],&_s,4); }");
                    return true;
                }
            }

            // One-source: FMOV/FABS/FNEG/FSQRT and the FRINT family (opcode in
            // bits 20..15 low bits).
            if (((i >> 10) & 0x1F) == 0x10) {
                const u32 opcode = (i >> 15) & 0x3F;
                std::string expr;
                switch (opcode) {
                case 0: expr = "_a"; break;                             // FMOV
                case 1: expr = dbl ? "fabs(_a)" : "fabsf(_a)"; break;   // FABS
                case 2: expr = "-_a"; break;                            // FNEG
                case 3: expr = dbl ? "sqrt(_a)" : "sqrtf(_a)"; break;   // FSQRT
                // FRINT* round to an integral value but stay in the FP format.
                // The rounding mode is part of the instruction except for
                // FRINTI, which follows FPCR; the generated code never changes
                // the host rounding mode, so FPCR is the default round-to-
                // nearest-even and rint() matches it. FRINTX differs from
                // FRINTN only in raising the inexact exception, which is not
                // modelled, so both map to rint().
                case 8:  expr = dbl ? "rint(_a)"  : "rintf(_a)";  break; // FRINTN
                case 9:  expr = dbl ? "ceil(_a)"  : "ceilf(_a)";  break; // FRINTP
                case 10: expr = dbl ? "floor(_a)" : "floorf(_a)"; break; // FRINTM
                case 11: expr = dbl ? "trunc(_a)" : "truncf(_a)"; break; // FRINTZ
                case 12: expr = dbl ? "round(_a)" : "roundf(_a)"; break; // FRINTA
                case 14: expr = dbl ? "rint(_a)"  : "rintf(_a)";  break; // FRINTX
                case 15: expr = dbl ? "rint(_a)"  : "rintf(_a)";  break; // FRINTI
                default: break;
                }
                if (!expr.empty()) {
                    put(ld_n + "(void)_b; _r = " + expr + "; " + st_d);
                    return true;
                }
            }

            // FCCMP / FCCMPE - the conditional compare. When the condition
            // holds it behaves as FCMP; when it does not, NZCV is loaded from
            // the instruction's own 4-bit immediate instead. Bits 11..10 are
            // 01 here, against 00 for FCMP, so the two do not overlap.
            if (((i >> 10) & 3) == 1) {
                const u32 cond = (i >> 12) & 15, nzcv = i & 15;
                std::string s = ld_n + ld_m;
                s += "if (recomp_cond(c," + std::to_string(cond) + ")) { ";
                s += "if (_a != _a || _b != _b) { c->n=0; c->z=0; c->c=1; c->v=1; } ";
                s += "else { c->n = (_a < _b); c->z = (_a == _b); "
                     "c->c = (_a >= _b); c->v = 0; } } else { ";
                s += "c->n=" + std::to_string((nzcv >> 3) & 1) + "; ";
                s += "c->z=" + std::to_string((nzcv >> 2) & 1) + "; ";
                s += "c->c=" + std::to_string((nzcv >> 1) & 1) + "; ";
                s += "c->v=" + std::to_string(nzcv & 1) + "; } ";
                s += "(void)_r; }";
                put(s);
                return true;
            }

            // FCMP / FCMPE, including the compare-against-zero forms. The
            // low five bits are opcode2: bit 3 selects the #0.0 variant (Rm is
            // then not a register at all) and bit 4 selects the signalling
            // form, which differs only in how it reports NaNs and so produces
            // the same flags here. Requiring all five to be clear, as before,
            // matched only a quarter of the compares in real code.
            if (((i >> 10) & 0xF) == 8 && ((i >> 14) & 3) == 0 && (i & 7) == 0) {
                const bool cmp_zero = ((i >> 3) & 1) != 0;
                std::string s = ld_n;
                if (cmp_zero) {
                    s += std::string("_b = (") + ct + ")0; ";
                } else {
                    s += ld_m;
                }
                // Unordered (either NaN) sets C and V per the architecture.
                s += "if (_a != _a || _b != _b) { c->n=0; c->z=0; c->c=1; c->v=1; } ";
                s += "else { c->n = (_a < _b); c->z = (_a == _b); "
                     "c->c = (_a >= _b); c->v = 0; } ";
                s += "(void)_r; }";
                put(s);
                return true;
            }
        }
    }

    // Data-processing 2-source: UDIV, SDIV, and variable-shift forms LSLV/LSRV/ASRV/RORV.
    // Encoding: sf 0 0 1 1 0 1 0 1 Rm opcode Rn Rd
    if ((i & 0x5FE00000) == 0x1AC00000) {
        const u32 sf = i >> 31;
        const u32 rm = (i >> 16) & 31, opcode = (i >> 10) & 63;
        const u32 rn = (i >> 5) & 31, rd = i & 31;
        if (rd != 31) {
            // Register 31 reads as XZR in this group, not SP.
            const std::string xn = Xz(rn);
            const std::string xm = Xz(rm);
            std::string s;
            switch (opcode) {
            case 2:  // UDIV - divide by 0 yields 0 per ARM spec
                if (sf) s = "{ uint64_t _r=" + xm + "?" + xn + "/" + xm + ":0; c->x[" + std::to_string(rd) + "]=_r; }";
                else    s = "{ uint32_t _a=(uint32_t)" + xn + ",_b=(uint32_t)" + xm + "; c->x[" + std::to_string(rd) + "]=(uint64_t)(_b?_a/_b:0); }";
                break;
            case 3:  // SDIV
                if (sf) s = "{ int64_t _a=(int64_t)" + xn + ",_b=(int64_t)" + xm + "; c->x[" + std::to_string(rd) + "]=(uint64_t)(_b?_a/_b:0); }";
                else    s = "{ int32_t _a=(int32_t)(uint32_t)" + xn + ",_b=(int32_t)(uint32_t)" + xm + "; c->x[" + std::to_string(rd) + "]=(uint64_t)(uint32_t)(_b?_a/_b:0); }";
                break;
            case 8:  // LSLV
                if (sf) s = "{ c->x[" + std::to_string(rd) + "]=" + xn + "<<(" + xm + "&63); }";
                else    s = "{ c->x[" + std::to_string(rd) + "]=(uint64_t)(uint32_t)((uint32_t)" + xn + "<<(" + xm + "&31)); }";
                break;
            case 9:  // LSRV
                if (sf) s = "{ c->x[" + std::to_string(rd) + "]=" + xn + ">>(" + xm + "&63); }";
                else    s = "{ c->x[" + std::to_string(rd) + "]=(uint64_t)((uint32_t)" + xn + ">>(" + xm + "&31)); }";
                break;
            case 10: // ASRV
                if (sf) s = "{ c->x[" + std::to_string(rd) + "]=(uint64_t)((int64_t)" + xn + ">>(" + xm + "&63)); }";
                else    s = "{ c->x[" + std::to_string(rd) + "]=(uint64_t)(uint32_t)((int32_t)(uint32_t)" + xn + ">>(" + xm + "&31)); }";
                break;
            case 16: case 17: case 18: case 19:   // CRC32B/H/W/X
            case 20: case 21: case 22: case 23: { // CRC32CB/CH/CW/CX
                // The reflected formulation: the accumulator is already in
                // bit-reversed order, so the architecture's BitReverse and
                // Poly32Mod2 collapse into the usual table-free byte loop.
                const u32 sz = opcode & 3;
                const bool castagnoli = (opcode & 4) != 0;
                const u32 nbytes = 1u << sz;
                // The 64-bit data form is the only one with sf set, and every
                // other size needs it clear; anything else is unallocated.
                if ((sz == 3) != (sf != 0)) break;
                const char* poly = castagnoli ? "0x82F63B78U" : "0xEDB88320U";
                s = "{ uint32_t _c=(uint32_t)" + xn + "; uint64_t _v=" + xm + "; ";
                s += "for(int _b=0;_b<" + std::to_string(nbytes) + ";_b++){ ";
                s += "_c ^= (uint32_t)((_v>>(8*_b))&0xFFU); ";
                s += "for(int _k=0;_k<8;_k++) _c = (_c>>1) ^ ((_c&1U) ? " +
                     std::string(poly) + " : 0U); } ";
                s += "c->x[" + std::to_string(rd) + "]=(uint64_t)_c; }";
                break;
            }
            case 11: // RORV
                if (sf) s = "{ uint64_t _a=" + xn + ",_s=" + xm + "&63; c->x[" + std::to_string(rd) + "]=_s?(_a>>_s)|(_a<<(64-_s)):_a; }";
                else    s = "{ uint32_t _a=(uint32_t)" + xn + ",_s=(uint32_t)" + xm + "&31; c->x[" + std::to_string(rd) + "]=(uint64_t)(uint32_t)(_s?(_a>>_s)|(_a<<(32-_s)):_a); }";
                break;
            }
            if (!s.empty()) { put(s); return true; }
        }
    }

    // Advanced SIMD two-register misc, floating-point compare against zero:
    // FCMGT, FCMGE, FCMEQ, FCMLE and FCMLT. A true lane is all-ones, which is
    // what the following select/AND normally consumes. Only the bit-23-clear
    // group is decoded; the rest stays on the fallback.
    {
        const bool vec_misc = (i & 0x9F3E0C00) == 0x0E200800;
        // Bit 29 is U and must stay out of the mask, or only the U=0 half of
        // each pair (FCMEQ but not FCMLE, FCMGT but not FCMGE) would match.
        const bool scl_misc = (i & 0xDF3E0C00) == 0x5E200800;
        if (vec_misc || scl_misc) {
            const u32 Q = (i >> 30) & 1, U = (i >> 29) & 1;
            const u32 opcode = (i >> 12) & 0x1F;
            const bool dbl = ((i >> 22) & 1) != 0;
            const u32 rn = (i >> 5) & 31, rd = i & 31;
            const char* cmp = nullptr;
            if (opcode == 0x0C) cmp = U ? ">=" : ">";      // FCMGE / FCMGT
            else if (opcode == 0x0D) cmp = U ? "<=" : "=="; // FCMLE / FCMEQ
            else if (opcode == 0x0E && !U) cmp = "<";       // FCMLT
            // SCVTF / UCVTF: convert the integer in each lane to a float of the
            // same width. This is the vector counterpart of the general-register
            // conversion handled further up - the operand is a lane here, not a
            // general register.
            const u32 a23 = (i >> 23) & 1;
            // FRECPE and FRSQRTE share opcode 11101 with SCVTF and UCVTF and
            // are told apart only by bit 23. The mask above deliberately leaves
            // that bit free, so without this test an FRECPE would be emitted as
            // an integer-to-float conversion - a different operation producing
            // plausible-looking garbage rather than an honest fallback.
            // FCVT{N,M,P,Z,A}{S,U}: converter cada faixa para inteiro com o
            // modo de arredondamento que o opcode nomeia. Bit 23 separa dois
            // pares distintos no mesmo opcode, tal como em SCVTF e FRECPE.
            if ((opcode == 0x1A || opcode == 0x1B || (opcode == 0x1C && !a23))) {
                const char* round_fn = nullptr;
                if (opcode == 0x1A) round_fn = a23 ? (dbl ? "ceil" : "ceilf")   // FCVTP
                                                   : (dbl ? "rint" : "rintf");  // FCVTN
                else if (opcode == 0x1B) round_fn = a23 ? (dbl ? "trunc" : "truncf")  // FCVTZ
                                                        : (dbl ? "floor" : "floorf"); // FCVTM
                else round_fn = dbl ? "round" : "roundf";                        // FCVTA
                const char* ct = dbl ? "double" : "float";
                const int fsz = dbl ? 8 : 4;
                const int bytes = scl_misc ? fsz : (Q ? 16 : 8);
                const int lanes = bytes / fsz;
                const std::string oty = (U ? "uint" : "int") + std::to_string(fsz * 8) + "_t";
                const char* lo = U ? "0.0" : (dbl ? "-9223372036854775808.0" : "-2147483648.0");
                const char* hi = U ? (dbl ? "18446744073709551615.0" : "4294967295.0")
                                   : (dbl ? "9223372036854775807.0" : "2147483647.0");
                const std::string sat_lo = U ? "0" : (dbl ? "(-9223372036854775807LL - 1)"
                                                          : "(-2147483647 - 1)");
                const std::string sat_hi = U ? (dbl ? "0xFFFFFFFFFFFFFFFFULL" : "0xFFFFFFFFU")
                                             : (dbl ? "0x7FFFFFFFFFFFFFFFLL" : "0x7FFFFFFF");
                std::string s = "{ " + std::string(ct) + " _a[" + std::to_string(lanes) + "]; " +
                                oty + " _r[" + std::to_string(lanes) + "]; ";
                s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "]," + std::to_string(bytes) + "); ";
                s += "for(int _i=0;_i<" + std::to_string(lanes) + ";_i++){ " + std::string(ct) +
                     " _v=" + std::string(round_fn) + "(_a[_i]); ";
                // Saturar como na forma escalar: NaN da zero, fora de alcance
                // encosta ao limite, que um cast simples nao faria.
                s += "if (_v != _v) _r[_i]=0; ";
                s += std::string("else if (!(_v > (") + ct + ")" + lo + ")) _r[_i]=(" + oty + ")" +
                     sat_lo + "; ";
                s += std::string("else if (!(_v < (") + ct + ")" + hi + ")) _r[_i]=(" + oty + ")" +
                     sat_hi + "; ";
                s += "else _r[_i]=(" + oty + ")_v; } ";
                s += "c->vreg[" + std::to_string(rd) + "][0]=0; c->vreg[" + std::to_string(rd) +
                     "][1]=0; memcpy(c->vreg[" + std::to_string(rd) + "],_r," +
                     std::to_string(bytes) + "); }";
                put(s);
                return true;
            }
            // FRINT* vetorial: arredondar para um valor inteiro mantendo o
            // formato. Bit 23 e U escolhem o modo, como nos FCVT* acima. O
            // codigo gerado nunca muda o modo do FPCR, por isso FRINTX e
            // FRINTI - que seguem o FPCR - ficam em nearest-even tal como
            // FRINTN; FRINTX so difere por levantar a excecao de inexacto.
            if (vec_misc && (opcode == 0x18 || opcode == 0x19)) {
                const char* fn = nullptr;
                if (!U) {
                    if (opcode == 0x18) fn = a23 ? (dbl ? "ceil" : "ceilf")      // FRINTP
                                                 : (dbl ? "rint" : "rintf");     // FRINTN
                    else                fn = a23 ? (dbl ? "trunc" : "truncf")    // FRINTZ
                                                 : (dbl ? "floor" : "floorf");   // FRINTM
                } else if (opcode == 0x18) {
                    if (!a23) fn = dbl ? "round" : "roundf";                     // FRINTA
                } else {
                    fn = dbl ? "rint" : "rintf";                                 // FRINTX/FRINTI
                }
                const int fsz = dbl ? 8 : 4;
                const int bytes = Q ? 16 : 8;
                const int lanes = bytes / fsz;
                if (fn && !(dbl && !Q)) {
                    const char* ct = dbl ? "double" : "float";
                    std::string s = "{ " + std::string(ct) + " _a[" + std::to_string(lanes) +
                                    "],_r[" + std::to_string(lanes) + "]; ";
                    s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "]," +
                         std::to_string(bytes) + "); ";
                    s += "for(int _i=0;_i<" + std::to_string(lanes) + ";_i++) _r[_i]=" +
                         std::string(fn) + "(_a[_i]); ";
                    s += "c->vreg[" + std::to_string(rd) + "][0]=0; c->vreg[" + std::to_string(rd) +
                         "][1]=0; memcpy(c->vreg[" + std::to_string(rd) + "],_r," +
                         std::to_string(bytes) + "); }";
                    put(s);
                    return true;
                }
            }
            // FCVTL/FCVTN entre single e double. So a forma bit-22-posto esta
            // aqui: a outra converte de e para half-precision, que este
            // tradutor nao representa, e fica no fallback em vez de adivinhada.
            if (vec_misc && !U && (opcode == 0x16 || opcode == 0x17) && dbl) {
                std::string s;
                if (opcode == 0x17) {
                    // FCVTL/FCVTL2: duas singles da metade que Q escolhe dao
                    // duas doubles, que ocupam o registo inteiro.
                    s = "{ float _a[2]; double _r[2]; ";
                    s += "memcpy(_a,(const uint8_t*)c->vreg[" + std::to_string(rn) + "]+" +
                         std::to_string(Q ? 8 : 0) + ",8); ";
                    s += "_r[0]=(double)_a[0]; _r[1]=(double)_a[1]; ";
                    s += "memcpy(c->vreg[" + std::to_string(rd) + "],_r,16); }";
                } else {
                    // FCVTN/FCVTN2: duas doubles dao duas singles; Q escolhe a
                    // metade do destino e a forma Q=1 preserva a outra.
                    s = "{ double _a[2]; float _r[2]; ";
                    s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "],16); ";
                    s += "_r[0]=(float)_a[0]; _r[1]=(float)_a[1]; ";
                    if (Q) s += "memcpy((uint8_t*)c->vreg[" + std::to_string(rd) + "]+8,_r,8); }";
                    else   s += "c->vreg[" + std::to_string(rd) + "][0]=0; c->vreg[" +
                                std::to_string(rd) + "][1]=0; memcpy(c->vreg[" +
                                std::to_string(rd) + "],_r,8); }";
                }
                put(s);
                return true;
            }
            if (opcode == 0x1D && a23) {
                const int fsz = dbl ? 8 : 4;
                const int bytes = scl_misc ? fsz : (Q ? 16 : 8);
                const int lanes = bytes / fsz;
                const std::string uty = "uint" + std::to_string(fsz * 8) + "_t";
                const char* fn = U ? "recomp_frsqrte" : "recomp_frecpe";
                std::string s = "{ " + uty + " _a[" + std::to_string(lanes) + "],_r[" +
                                std::to_string(lanes) + "]; ";
                s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "]," + std::to_string(bytes) + "); ";
                s += "for(int _i=0;_i<" + std::to_string(lanes) + ";_i++) _r[_i]=(" + uty + ")" +
                     std::string(fn) + "(_a[_i]," + std::string(dbl ? "1" : "0") + "); ";
                s += "c->vreg[" + std::to_string(rd) + "][0]=0; c->vreg[" + std::to_string(rd) +
                     "][1]=0; memcpy(c->vreg[" + std::to_string(rd) + "],_r," +
                     std::to_string(bytes) + "); }";
                put(s);
                return true;
            }
            if (opcode == 0x1D && !a23) {
                const char* ct = dbl ? "double" : "float";
                const int fsz = dbl ? 8 : 4;
                const int bytes = scl_misc ? fsz : (Q ? 16 : 8);
                const int lanes = bytes / fsz;
                const std::string ity = std::string(U ? "uint" : "int") +
                                        std::to_string(fsz * 8) + "_t";
                std::string s = "{ " + ity + " _a[" + std::to_string(lanes) + "]; " +
                                std::string(ct) + " _r[" + std::to_string(lanes) + "]; ";
                s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "]," + std::to_string(bytes) + "); ";
                s += "for(int _i=0;_i<" + std::to_string(lanes) + ";_i++) _r[_i]=(" +
                     std::string(ct) + ")_a[_i]; ";
                s += "c->vreg[" + std::to_string(rd) + "][0]=0; c->vreg[" + std::to_string(rd) +
                     "][1]=0; ";
                s += "memcpy(c->vreg[" + std::to_string(rd) + "],_r," + std::to_string(bytes) + "); }";
                put(s);
                return true;
            }
            if (cmp) {
                const char* ct = dbl ? "double" : "float";
                const int fsz = dbl ? 8 : 4;
                // A scalar form touches one lane; a vector form covers the
                // whole selected width.
                const int bytes = scl_misc ? fsz : (Q ? 16 : 8);
                const int lanes = bytes / fsz;
                const std::string uty = "uint" + std::to_string(fsz * 8) + "_t";
                std::string s = "{ " + std::string(ct) + " _a[" + std::to_string(lanes) + "]; " +
                                uty + " _r[" + std::to_string(lanes) + "]; ";
                s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "]," + std::to_string(bytes) + "); ";
                s += "for(int _i=0;_i<" + std::to_string(lanes) + ";_i++) _r[_i]=(_a[_i]" + cmp +
                     "(" + ct + ")0) ? (" + uty + ")~(" + uty + ")0 : (" + uty + ")0; ";
                s += "c->vreg[" + std::to_string(rd) + "][0]=0; c->vreg[" + std::to_string(rd) +
                     "][1]=0; ";
                s += "memcpy(c->vreg[" + std::to_string(rd) + "],_r," + std::to_string(bytes) + "); }";
                put(s);
                return true;
            }
        }
    }

    // Advanced SIMD two-register misc: the vector FABS and FNEG. These sit in
    // the half of the group selected by bit 23, where the remaining size bit
    // is the element width, so they cannot share the integer decode below.
    if ((i & 0x9FBE0C00) == 0x0EA00800) {
        const u32 Q = (i >> 30) & 1, U = (i >> 29) & 1;
        const u32 misc_op = (i >> 12) & 0x1F;
        const bool dbl = ((i >> 22) & 1) != 0;
        const u32 rn = (i >> 5) & 31, rd = i & 31;
        const int fsz = dbl ? 8 : 4;
        const int bytes = Q ? 16 : 8;
        const int lanes = bytes / fsz;
        if (!(dbl && !Q) && (misc_op == 0x0F || (misc_op == 0x1F && U))) {
            const char* ct = dbl ? "double" : "float";
            // opcode 01111 is FABS (U=0) and FNEG (U=1); opcode 11111 with U
            // set is FSQRT.
            const std::string expr =
                misc_op == 0x1F ? (dbl ? "sqrt(_a[_i])" : "sqrtf(_a[_i])")
                                : (U ? "-_a[_i]" : (dbl ? "fabs(_a[_i])" : "fabsf(_a[_i])"));
            std::string s = "{ " + std::string(ct) + " _a[" + std::to_string(lanes) + "],_r[" +
                            std::to_string(lanes) + "]; ";
            s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "]," + std::to_string(bytes) + "); ";
            s += "for(int _i=0;_i<" + std::to_string(lanes) + ";_i++) _r[_i]=" + expr + "; ";
            s += "c->vreg[" + std::to_string(rd) + "][0]=0; c->vreg[" + std::to_string(rd) +
                 "][1]=0; memcpy(c->vreg[" + std::to_string(rd) + "],_r," +
                 std::to_string(bytes) + "); }";
            put(s);
            return true;
        }
    }

    // Advanced SIMD two-register misc, the narrowing forms: XTN, SQXTN, UQXTN
    // and SQXTUN. The source elements are twice the width of the destination's,
    // and Q picks which half of Vd the result lands in - so the Q=1 form reads
    // Vd back rather than replacing it.
    if ((i & 0x9F3FFC00) == 0x0E212800 || (i & 0x9F3FFC00) == 0x0E214800) {
        const u32 Q = (i >> 30) & 1, U = (i >> 29) & 1;
        const u32 size = (i >> 22) & 3, opcode = (i >> 12) & 0x1F;
        const u32 rn = (i >> 5) & 31, rd = i & 31;
        if (size < 3) {
            const u32 esize = 8u << size;
            const int lanes = 8 / (int)(esize / 8);
            const std::string dty = "uint" + std::to_string(esize) + "_t";
            const std::string sty = "uint" + std::to_string(esize * 2) + "_t";
            const std::string ssty = "int" + std::to_string(esize * 2) + "_t";
            std::string conv;
            if (opcode == 0x12 && !U) {
                conv = "_r[_i]=(" + dty + ")_a[_i];";           // XTN, plain truncation
            } else if (opcode == 0x12) {
                // SQXTUN: a signed source saturated into an unsigned result.
                conv = "{ " + ssty + " _v=(" + ssty + ")_a[_i]; _r[_i] = _v < 0 ? (" + dty +
                       ")0 : (_v > (" + ssty + ")(" + dty + ")~(" + dty + ")0 ? (" + dty +
                       ")~(" + dty + ")0 : (" + dty + ")_v); }";
            } else if (!U) {
                // SQXTN: signed source, signed result.
                const std::string smax = "(" + ssty + ")((" + dty + ")~(" + dty + ")0 >> 1)";
                const std::string smin = "(" + ssty + ")(-" + smax + " - 1)";
                conv = "{ " + ssty + " _v=(" + ssty + ")_a[_i]; _r[_i] = (" + dty + ")(_v > " +
                       smax + " ? " + smax + " : (_v < " + smin + " ? " + smin + " : _v)); }";
            } else {
                // UQXTN: unsigned source, unsigned result.
                conv = "{ " + sty + " _v=_a[_i]; _r[_i] = _v > (" + sty + ")(" + dty + ")~(" +
                       dty + ")0 ? (" + dty + ")~(" + dty + ")0 : (" + dty + ")_v; }";
            }
            std::string s = "{ " + sty + " _a[" + std::to_string(lanes) + "]; " + dty + " _r[" +
                            std::to_string(lanes) + "]; ";
            s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "],16); ";
            s += "for(int _i=0;_i<" + std::to_string(lanes) + ";_i++) " + conv + " ";
            if (Q) {
                // XTN2 and friends keep the low half of the destination.
                s += "memcpy((uint8_t*)c->vreg[" + std::to_string(rd) + "]+8,_r,8); }";
            } else {
                s += "c->vreg[" + std::to_string(rd) + "][0]=0; c->vreg[" + std::to_string(rd) +
                     "][1]=0; memcpy(c->vreg[" + std::to_string(rd) + "],_r,8); }";
            }
            put(s);
            return true;
        }
    }

    // PMULL/PMULL2: multiplicacao polinomial em GF(2), sem transporte entre
    // bits. Estao definidas so duas larguras - oito faixas de 8 bits para
    // dezesseis, ou uma de 64 bits para 128 - e Q escolhe de que metade dos
    // operandos vem a fonte.
    if ((i & 0xBF20FC00) == 0x0E20E000) {
        const u32 Q = (i >> 30) & 1, size = (i >> 22) & 3;
        const u32 rm = (i >> 16) & 31, rn = (i >> 5) & 31, rd = i & 31;
        const std::string off = std::to_string(Q ? 8 : 0);
        if (size == 3) {
            std::string s = "{ uint64_t _a,_b,_lo=0,_hi=0; ";
            s += "memcpy(&_a,(const uint8_t*)c->vreg[" + std::to_string(rn) + "]+" + off + ",8); ";
            s += "memcpy(&_b,(const uint8_t*)c->vreg[" + std::to_string(rm) + "]+" + off + ",8); ";
            // Cada bit posto em _b desloca _a e soma em GF(2), ou seja XOR. O
            // deslocamento de 64 - _i seria indefinido para _i igual a zero,
            // por isso a metade alta so entra quando ha algo para levar.
            s += "for(int _i=0;_i<64;_i++) if((_b>>_i)&1){ _lo ^= _a << _i; ";
            s += "if(_i) _hi ^= _a >> (64-_i); } ";
            s += "c->vreg[" + std::to_string(rd) + "][0]=_lo; c->vreg[" + std::to_string(rd) +
                 "][1]=_hi; }";
            put(s);
            return true;
        }
        if (size == 0) {
            std::string s = "{ uint8_t _a[8],_b[8]; uint16_t _r[8]; ";
            s += "memcpy(_a,(const uint8_t*)c->vreg[" + std::to_string(rn) + "]+" + off + ",8); ";
            s += "memcpy(_b,(const uint8_t*)c->vreg[" + std::to_string(rm) + "]+" + off + ",8); ";
            s += "for(int _i=0;_i<8;_i++){ uint16_t _v=0; ";
            s += "for(int _j=0;_j<8;_j++) if((_b[_i]>>_j)&1) _v ^= (uint16_t)((uint16_t)_a[_i] << _j); ";
            s += "_r[_i]=_v; } ";
            s += "memcpy(c->vreg[" + std::to_string(rd) + "],_r,16); }";
            put(s);
            return true;
        }
    }

    // Advanced SIMD three-register different: o grupo de alargamento. Os
    // operandos tem metade da largura do resultado, e Q escolhe se vem da
    // metade baixa ou alta dos registos fonte. As formas "wide" (W) usam Rn ja
    // na largura do resultado e so alargam Rm.
    if ((i & 0x9F200C00) == 0x0E200000) {
        const u32 Q = (i >> 30) & 1, U = (i >> 29) & 1;
        const u32 size = (i >> 22) & 3, opcode = (i >> 12) & 15;
        const u32 rm = (i >> 16) & 31, rn = (i >> 5) & 31, rd = i & 31;
        const bool wide = (opcode == 1 || opcode == 3);        // SADDW / SSUBW
        const bool sub = (opcode == 2 || opcode == 3);         // SSUBL / SSUBW
        const bool mul = (opcode == 8 || opcode == 10 || opcode == 12);
        const bool acc = (opcode == 8 || opcode == 10);        // SMLAL / SMLSL
        const bool msub = (opcode == 10);                      // SMLSL
        const bool known = (opcode == 0 || opcode == 1 || opcode == 2 || opcode == 3 || mul);
        if (known && size < 3) {
            const u32 esize = 8u << size;              // largura da fonte
            const int lanes = 8 / (int)(esize / 8);    // faixas do resultado
            const std::string sty = "uint" + std::to_string(esize) + "_t";
            const std::string wty = "uint" + std::to_string(esize * 2) + "_t";
            const std::string wid = U ? ("(" + wty + ")")
                                      : ("(" + wty + ")(int" + std::to_string(esize * 2) +
                                         "_t)(int" + std::to_string(esize) + "_t)");
            std::string s = "{ " + sty + " _m[" + std::to_string(lanes) + "]; " + wty + " _a[" +
                            std::to_string(lanes) + "],_r[" + std::to_string(lanes) + "]; ";
            // Q seleciona a metade dos operandos estreitos.
            s += "memcpy(_m,(const uint8_t*)c->vreg[" + std::to_string(rm) + "]+" +
                 std::to_string(Q ? 8 : 0) + ",8); ";
            if (wide) {
                s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "],16); ";
            } else {
                s += "{ " + sty + " _n[" + std::to_string(lanes) + "]; ";
                s += "memcpy(_n,(const uint8_t*)c->vreg[" + std::to_string(rn) + "]+" +
                     std::to_string(Q ? 8 : 0) + ",8); ";
                s += "for(int _i=0;_i<" + std::to_string(lanes) + ";_i++) _a[_i]=" + wid + "_n[_i]; } ";
            }
            if (acc) s += "memcpy(_r,c->vreg[" + std::to_string(rd) + "],16); ";
            s += "for(int _i=0;_i<" + std::to_string(lanes) + ";_i++) ";
            if (mul) {
                // Duas faixas de 16 bits promovem a int, onde o produto
                // transborda; alargar antes de multiplicar evita isso.
                const std::string prod = "(" + wty + ")((uint64_t)(" + wid +
                                         "_m[_i]) * (uint64_t)_a[_i])";
                if (acc) s += "_r[_i]=(" + wty + ")(_r[_i] " + (msub ? "-" : "+") + " " + prod + "); ";
                else     s += "_r[_i]=" + prod + "; ";
            } else {
                s += "_r[_i]=(" + wty + ")(_a[_i] " + std::string(sub ? "-" : "+") + " " + wid +
                     "_m[_i]); ";
            }
            s += "memcpy(c->vreg[" + std::to_string(rd) + "],_r,16); }";
            put(s);
            return true;
        }
    }

    // Extensao de criptografia SHA-256.
    if ((i & 0xFFE0FC00) == 0x5E004000 || (i & 0xFFE0FC00) == 0x5E005000 ||
        (i & 0xFFE0FC00) == 0x5E006000) {
        const u32 rm = (i >> 16) & 31, rn = (i >> 5) & 31, rd = i & 31;
        const u32 kind = (i >> 12) & 7;   // 4 H, 5 H2, 6 SU1
        const char* fn = kind == 4 ? "recomp_sha256h"
                       : kind == 5 ? "recomp_sha256h2" : "recomp_sha256su1";
        put(std::string(fn) + "(c," + std::to_string(rd) + "," + std::to_string(rn) + "," +
            std::to_string(rm) + ");");
        return true;
    }
    if ((i & 0xFFFFFC00) == 0x5E282800) {
        const u32 rn = (i >> 5) & 31, rd = i & 31;
        put("recomp_sha256su0(c," + std::to_string(rd) + "," + std::to_string(rn) + ");");
        return true;
    }

    // Extensao de criptografia SHA-1.
    if ((i & 0xFFE0FC00) == 0x5E000000 || (i & 0xFFE0FC00) == 0x5E001000 ||
        (i & 0xFFE0FC00) == 0x5E002000 || (i & 0xFFE0FC00) == 0x5E003000) {
        const u32 rm = (i >> 16) & 31, rn = (i >> 5) & 31, rd = i & 31;
        const u32 kind = (i >> 12) & 3;   // 0 SHA1C, 1 SHA1P, 2 SHA1M, 3 SHA1SU0
        if (kind == 3) {
            put("recomp_sha1su0(c," + std::to_string(rd) + "," + std::to_string(rn) + "," +
                std::to_string(rm) + ");");
        } else {
            const int fn = kind == 0 ? 0 : kind == 1 ? 2 : 1;   // choose, parity, majority
            put("recomp_sha1_hash(c," + std::to_string(rd) + "," + std::to_string(rn) + "," +
                std::to_string(rm) + "," + std::to_string(fn) + ");");
        }
        return true;
    }
    if ((i & 0xFFFFFC00) == 0x5E280800 || (i & 0xFFFFFC00) == 0x5E281800) {
        const u32 rn = (i >> 5) & 31, rd = i & 31;
        const bool su1 = ((i >> 12) & 1) != 0;
        put(std::string(su1 ? "recomp_sha1su1(c," : "recomp_sha1h(c,") + std::to_string(rd) +
            "," + std::to_string(rn) + ");");
        return true;
    }

    // Extensao de criptografia AES. Encodings fixos, sem campos de tamanho:
    // operam sempre sobre os 128 bits inteiros.
    if ((i & 0xFFFFFC00) == 0x4E284800 || (i & 0xFFFFFC00) == 0x4E285800 ||
        (i & 0xFFFFFC00) == 0x4E286800 || (i & 0xFFFFFC00) == 0x4E287800) {
        const u32 rn = (i >> 5) & 31, rd = i & 31;
        const u32 which = (i >> 12) & 3;   // 4=AESE 5=AESD 6=AESMC 7=AESIMC
        const char* fn = which == 0 ? "recomp_aese" : which == 1 ? "recomp_aesd"
                       : which == 2 ? "recomp_aesmc" : "recomp_aesimc";
        put(std::string(fn) + "(c," + std::to_string(rd) + "," + std::to_string(rn) + ");");
        return true;
    }

    // Advanced SIMD two-register misc, integer forms: CNT, NOT, ABS and NEG.
    // Same encoding group as the floating-point compares above, selected by a
    // different opcode, so only the vector form is decoded here - the scalar
    // 0x5E... variants stay on the fallback.
    if ((i & 0x9F3E0C00) == 0x0E200800) {
        const u32 Q = (i >> 30) & 1, U = (i >> 29) & 1;
        const u32 size = (i >> 22) & 3, opcode = (i >> 12) & 0x1F;
        const u32 rn = (i >> 5) & 31, rd = i & 31;
        const int bytes = Q ? 16 : 8;
        // A 64-bit element with Q clear would be a single lane, which this
        // group does not define.
        const bool width_ok = !(size == 3 && !Q);
        const std::string uty = "uint" + std::to_string(8 << size) + "_t";
        const std::string ity = "int" + std::to_string(8 << size) + "_t";
        const int lanes = bytes / (1 << size);
        std::string body;
        if (opcode == 0x05 && size == 0) {
            // CNT counts the set bits of each byte; NOT shares the opcode and
            // is selected by U. Both are byte-wise regardless of the size
            // field, which must be 00 for either.
            if (!U) body = "for(int _i=0;_i<" + std::to_string(bytes) +
                           ";_i++){ uint8_t _v=_a[_i]; _v=(uint8_t)(_v-((_v>>1)&0x55)); "
                           "_v=(uint8_t)((_v&0x33)+((_v>>2)&0x33)); "
                           "_r[_i]=(uint8_t)((_v+(_v>>4))&0x0f); } ";
            else      body = "for(int _i=0;_i<" + std::to_string(bytes) +
                             ";_i++) _r[_i]=(uint8_t)~_a[_i]; ";
        } else if (opcode == 0x13 && U && size < 3) {
            // SHLL desloca cada faixa para a esquerda pela sua propria largura,
            // alargando-a ao dobro. Tratado aqui em vez de com os
            // deslocamentos por imediato porque o deslocamento nao vem de um
            // campo: e sempre igual a largura do elemento.
            const u32 esize = 8u << size;
            const std::string sty = "uint" + std::to_string(esize) + "_t";
            const std::string wty = "uint" + std::to_string(esize * 2) + "_t";
            const int n = 8 / (int)(esize / 8);
            std::string s = "{ " + sty + " _s[" + std::to_string(n) + "]; " + wty + " _w[" +
                            std::to_string(n) + "]; ";
            s += "memcpy(_s,(const uint8_t*)c->vreg[" + std::to_string(rn) + "]+" +
                 std::to_string(Q ? 8 : 0) + ",8); ";
            s += "for(int _i=0;_i<" + std::to_string(n) + ";_i++) _w[_i]=(" + wty + ")((" + wty +
                 ")_s[_i] << " + std::to_string(esize) + "); ";
            s += "memcpy(c->vreg[" + std::to_string(rd) + "],_w,16); }";
            put(s);
            return true;
        } else if ((opcode == 0x02 || opcode == 0x06) && size < 3) {
            // SADDLP/UADDLP somam pares de faixas vizinhas para o dobro da
            // largura; SADALP/UADALP acumulam esse resultado no destino.
            const u32 esize = 8u << size;
            const bool accum = (opcode == 0x06);
            const int bytes = Q ? 16 : 8;
            const int pairs = bytes / (int)(esize / 8) / 2;
            const std::string sty = "uint" + std::to_string(esize) + "_t";
            const std::string wty = "uint" + std::to_string(esize * 2) + "_t";
            const std::string wid = U ? ("(" + wty + ")")
                                      : ("(" + wty + ")(int" + std::to_string(esize * 2) +
                                         "_t)(int" + std::to_string(esize) + "_t)");
            std::string s = "{ " + sty + " _s[" + std::to_string(pairs * 2) + "]; " + wty + " _w[" +
                            std::to_string(pairs) + "]; ";
            s += "memcpy(_s,c->vreg[" + std::to_string(rn) + "]," + std::to_string(bytes) + "); ";
            if (accum) s += "memcpy(_w,c->vreg[" + std::to_string(rd) + "]," +
                            std::to_string(pairs * (int)(esize / 4)) + "); ";
            s += "for(int _i=0;_i<" + std::to_string(pairs) + ";_i++) _w[_i]=(" + wty + ")(" +
                 (accum ? "_w[_i] + " : "") + wid + "_s[2*_i] + " + wid + "_s[2*_i+1]); ";
            s += "c->vreg[" + std::to_string(rd) + "][0]=0; c->vreg[" + std::to_string(rd) +
                 "][1]=0; memcpy(c->vreg[" + std::to_string(rd) + "],_w," +
                 std::to_string(pairs * (int)(esize / 4)) + "); }";
            put(s);
            return true;
        } else if ((opcode == 0x00 || opcode == 0x01) && width_ok) {
            // REV64, REV32 and REV16 reverse the element order inside each
            // container: a doubleword for REV64, a word for REV32, a halfword
            // for REV16. The container is what the opcode names, the element
            // is what the size field names.
            // opcode 00000 e REV64 com U=0 e REV32 com U=1; opcode 00001 e
            // REV16, que so existe com U=0.
            const u32 container = opcode == 0x00 ? (U ? 32u : 64u) : 16u;
            const bool rev_ok = (opcode == 0x00 || !U) && (8u << size) < container;
            if (rev_ok) {
                const u32 per = container / (8u << size);
                body = "for(int _g=0;_g<" + std::to_string(lanes / (int)per) + ";_g++) "
                       "for(int _i=0;_i<" + std::to_string(per) + ";_i++) "
                       "_r[_g*" + std::to_string(per) + "+_i]=_a[_g*" + std::to_string(per) +
                       "+" + std::to_string(per - 1) + "-_i]; ";
            }
        } else if (opcode == 0x04 && width_ok && size < 3) {
            // CLS counts the leading bits that match the sign, not counting the
            // sign itself; CLZ counts leading zeros.
            const u32 esize = 8u << size;
            if (U) body = "for(int _i=0;_i<" + std::to_string(lanes) + ";_i++){ " + uty +
                          " _v=_a[_i]; int _n=0; while(_n<" + std::to_string(esize) +
                          " && !((_v>>(" + std::to_string(esize - 1) + "-_n))&1)) _n++; "
                          "_r[_i]=(" + uty + ")_n; } ";
            else   body = "for(int _i=0;_i<" + std::to_string(lanes) + ";_i++){ " + uty +
                          " _v=_a[_i]; int _n=0; while(_n<" + std::to_string(esize - 1) +
                          " && (((_v>>(" + std::to_string(esize - 1) + "-_n))&1)==((_v>>(" +
                          std::to_string(esize - 2) + "-_n))&1))) _n++; _r[_i]=(" + uty + ")_n; } ";
        } else if ((opcode == 0x08 || opcode == 0x09 || opcode == 0x0A) && width_ok) {
            // The compares against zero. A true lane is all-ones.
            const char* zcmp = nullptr;
            if (opcode == 0x08) zcmp = U ? ">=" : ">";
            else if (opcode == 0x09) zcmp = U ? "<=" : "==";
            else if (!U) zcmp = "<";
            if (zcmp) {
                body = "for(int _i=0;_i<" + std::to_string(lanes) + ";_i++) _r[_i]=((" + ity +
                       ")_a[_i] " + std::string(zcmp) + " 0) ? (" + uty + ")~(" + uty +
                       ")0 : (" + uty + ")0; ";
            }
        } else if (opcode == 0x0B && width_ok) {
            // ABS and NEG. Both are done on the unsigned type after an
            // explicit cast, so the INT_MIN case wraps as the architecture
            // requires instead of being undefined.
            if (!U) body = "for(int _i=0;_i<" + std::to_string(lanes) +
                           ";_i++){ " + ity + " _v=(" + ity + ")_a[_i]; "
                           "_r[_i] = _v < 0 ? (" + uty + ")(0-_a[_i]) : _a[_i]; } ";
            else      body = "for(int _i=0;_i<" + std::to_string(lanes) +
                             ";_i++) _r[_i]=(" + uty + ")(0-_a[_i]); ";
        }
        if (!body.empty()) {
            const std::string ety = (opcode == 0x05) ? std::string("uint8_t") : uty;
            const int count = (opcode == 0x05) ? bytes : lanes;
            (void)ity;
            std::string s = "{ " + ety + " _a[" + std::to_string(count) + "],_r[" +
                            std::to_string(count) + "]; ";
            s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "]," + std::to_string(bytes) + "); ";
            s += body;
            s += "c->vreg[" + std::to_string(rd) + "][0]=0; c->vreg[" + std::to_string(rd) +
                 "][1]=0; memcpy(c->vreg[" + std::to_string(rd) + "],_r," +
                 std::to_string(bytes) + "); }";
            put(s);
            return true;
        }
    }

    // Advanced SIMD EXT: a byte-aligned window over the concatenation Vn:Vm.
    // This is how vector code rotates or realigns a register, so it turns up
    // wherever data is shifted by a whole number of bytes.
    if ((i & 0xBFE08400) == 0x2E000000) {
        const u32 Q = (i >> 30) & 1;
        const u32 rm = (i >> 16) & 31, imm4 = (i >> 11) & 15;
        const u32 rn = (i >> 5) & 31, rd = i & 31;
        const int bytes = Q ? 16 : 8;
        // With Q clear the window is eight bytes wide, so the top bit of imm4
        // has no meaning and that encoding is not allocated.
        if ((u32)imm4 < (u32)bytes) {
            std::string s = "{ uint8_t _c[" + std::to_string(bytes * 2) + "],_r[" +
                            std::to_string(bytes) + "]; ";
            s += "memcpy(_c,c->vreg[" + std::to_string(rn) + "]," + std::to_string(bytes) + "); ";
            s += "memcpy(_c+" + std::to_string(bytes) + ",c->vreg[" + std::to_string(rm) +
                 "]," + std::to_string(bytes) + "); ";
            s += "memcpy(_r,_c+" + std::to_string(imm4) + "," + std::to_string(bytes) + "); ";
            s += "c->vreg[" + std::to_string(rd) + "][0]=0; c->vreg[" + std::to_string(rd) +
                 "][1]=0; memcpy(c->vreg[" + std::to_string(rd) + "],_r," +
                 std::to_string(bytes) + "); }";
            put(s);
            return true;
        }
    }

    // Advanced SIMD table lookup: TBL and TBX. The table is one to four
    // consecutive registers starting at Vn, wrapping at the end of the
    // register file. An index past the end of the table gives zero for TBL and
    // leaves the destination byte untouched for TBX.
    if ((i & 0xBFE08C00) == 0x0E000000) {
        const u32 Q = (i >> 30) & 1;
        const u32 rm = (i >> 16) & 31, len = (i >> 13) & 3, op = (i >> 12) & 1;
        const u32 rn = (i >> 5) & 31, rd = i & 31;
        const int bytes = Q ? 16 : 8;
        const int tsize = (int)(len + 1) * 16;
        std::string s = "{ uint8_t _t[" + std::to_string(tsize) + "],_x[" +
                        std::to_string(bytes) + "],_r[" + std::to_string(bytes) + "]; ";
        for (u32 k = 0; k <= len; ++k) {
            s += "memcpy(_t+" + std::to_string(k * 16) + ",c->vreg[" +
                 std::to_string((rn + k) & 31) + "],16); ";
        }
        s += "memcpy(_x,c->vreg[" + std::to_string(rm) + "]," + std::to_string(bytes) + "); ";
        // TBX keeps the bytes it does not replace, so it has to read Vd first.
        if (op) s += "memcpy(_r,c->vreg[" + std::to_string(rd) + "]," + std::to_string(bytes) + "); ";
        else    s += "memset(_r,0," + std::to_string(bytes) + "); ";
        s += "for(int _i=0;_i<" + std::to_string(bytes) + ";_i++) if (_x[_i] < " +
             std::to_string(tsize) + ") _r[_i]=_t[_x[_i]]; ";
        s += "c->vreg[" + std::to_string(rd) + "][0]=0; c->vreg[" + std::to_string(rd) +
             "][1]=0; memcpy(c->vreg[" + std::to_string(rd) + "],_r," +
             std::to_string(bytes) + "); }";
        put(s);
        return true;
    }

    // Advanced SIMD permute: ZIP1/ZIP2, UZP1/UZP2 and TRN1/TRN2. Vector code
    // uses these to interleave and deinterleave lanes, so they turn up in any
    // hand-written maths routine.
    if ((i & 0xBF208C00) == 0x0E000800) {
        const u32 Q = (i >> 30) & 1, size = (i >> 22) & 3, opcode = (i >> 12) & 7;
        const u32 rm = (i >> 16) & 31, rn = (i >> 5) & 31, rd = i & 31;
        const int bytes = Q ? 16 : 8;
        const int lanes = bytes / (1 << size);
        const std::string uty = "uint" + std::to_string(8 << size) + "_t";
        // A single-lane result is not a permute; the 64-bit element form needs
        // the full 128-bit width.
        if (!(size == 3 && !Q) && lanes >= 2) {
            const int half = lanes / 2;
            std::string body;
            switch (opcode) {
            case 1: // UZP1 - the even-numbered lanes of Vn then of Vm
            case 5: // UZP2 - the odd-numbered lanes
                body = "for(int _i=0;_i<" + std::to_string(half) + ";_i++){ _r[_i]=_a[2*_i+" +
                       std::to_string(opcode == 5 ? 1 : 0) + "]; _r[_i+" + std::to_string(half) +
                       "]=_b[2*_i+" + std::to_string(opcode == 5 ? 1 : 0) + "]; } ";
                break;
            case 2: // TRN1 - even lanes of Vn interleaved with even lanes of Vm
            case 6: // TRN2 - the odd lanes
                body = "for(int _i=0;_i<" + std::to_string(half) + ";_i++){ _r[2*_i]=_a[2*_i+" +
                       std::to_string(opcode == 6 ? 1 : 0) + "]; _r[2*_i+1]=_b[2*_i+" +
                       std::to_string(opcode == 6 ? 1 : 0) + "]; } ";
                break;
            case 3: // ZIP1 - the lower half of each operand, interleaved
            case 7: // ZIP2 - the upper half
                body = "for(int _i=0;_i<" + std::to_string(half) + ";_i++){ _r[2*_i]=_a[_i+" +
                       std::to_string(opcode == 7 ? half : 0) + "]; _r[2*_i+1]=_b[_i+" +
                       std::to_string(opcode == 7 ? half : 0) + "]; } ";
                break;
            default: break;
            }
            if (!body.empty()) {
                std::string s = "{ " + uty + " _a[" + std::to_string(lanes) + "],_b[" +
                                std::to_string(lanes) + "],_r[" + std::to_string(lanes) + "]; ";
                s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "]," + std::to_string(bytes) + "); ";
                s += "memcpy(_b,c->vreg[" + std::to_string(rm) + "]," + std::to_string(bytes) + "); ";
                s += body;
                s += "c->vreg[" + std::to_string(rd) + "][0]=0; c->vreg[" + std::to_string(rd) +
                     "][1]=0; memcpy(c->vreg[" + std::to_string(rd) + "],_r," +
                     std::to_string(bytes) + "); }";
                put(s);
                return true;
            }
        }
    }

    // Advanced SIMD modified immediate: MOVI, MVNI and the vector FMOV. immh
    // (bits 22..19) is zero here, which is what separates this group from the
    // shift-by-immediate forms below that share the same major encoding.
    if ((i & 0x9FF80C00) == 0x0F000400) {
        const u32 Q = (i >> 30) & 1, op = (i >> 29) & 1, cmode = (i >> 12) & 15;
        const u32 imm8 = (((i >> 16) & 7) << 5) | ((i >> 5) & 0x1F);
        const u32 rd = i & 31;
        // AdvSIMDExpandImm, reduced to the 64-bit pattern each lane repeats.
        bool valid = true, invert = false, logical = false;
        u64 imm64 = 0;
        auto rep32 = [](u32 v) { return ((u64)v << 32) | v; };
        if ((cmode & 9) == 0) {                     // 0xx0: 32-bit, byte-shifted
            imm64 = rep32(imm8 << (8 * ((cmode >> 1) & 3)));
            invert = op != 0;
        } else if ((cmode & 13) == 8) {             // 10x0: 16-bit, byte-shifted
            const u32 imm16 = imm8 << (8 * ((cmode >> 1) & 1));
            imm64 = rep32((imm16 << 16) | imm16);
            invert = op != 0;
        } else if ((cmode & 14) == 12) {            // 110x: 32-bit, ones shifted in
            const u32 shift = 8 * (1 + (cmode & 1));
            imm64 = rep32((imm8 << shift) | ((1u << shift) - 1));
            invert = op != 0;
        } else if ((cmode & 9) == 1) {              // xxx1: ORR/BIC, 32-bit
            imm64 = rep32(imm8 << (8 * ((cmode >> 1) & 3)));
            logical = true;
            invert = op != 0;                       // BIC clears the set bits
        } else if ((cmode & 13) == 9) {             // 10x1: ORR/BIC, 16-bit
            const u32 imm16 = imm8 << (8 * ((cmode >> 1) & 1));
            imm64 = rep32((imm16 << 16) | imm16);
            logical = true;
            invert = op != 0;
        } else if (cmode == 14 && op == 0) {        // MOVI, byte-replicated
            u64 v = 0;
            for (int b = 0; b < 8; ++b) v |= (u64)imm8 << (b * 8);
            imm64 = v;
        } else if (cmode == 14 && op == 1) {        // MOVI 64-bit, bit-expanded
            u64 v = 0;
            for (int b = 0; b < 8; ++b) if ((imm8 >> b) & 1) v |= 0xFFULL << (b * 8);
            imm64 = v;
        } else if (cmode == 15) {                   // vector FMOV
            // Same VFPExpandImm as the scalar FMOV immediate: build the double
            // form and narrow it for the single-precision variant.
            const u32 sgn = (imm8 >> 7) & 1, b6 = (imm8 >> 6) & 1;
            const u64 e11 = ((u64)(b6 ^ 1) << 10) | (b6 ? (0xFFULL << 2) : 0ULL) |
                            ((imm8 >> 4) & 3);
            const u64 dbits = ((u64)sgn << 63) | (e11 << 52) | ((u64)(imm8 & 0xF) << 48);
            if (op == 0) {
                double dv; memcpy(&dv, &dbits, 8);
                const float fv = (float)dv;
                u32 fbits; memcpy(&fbits, &fv, 4);
                imm64 = rep32(fbits);
            } else if (Q) {
                imm64 = dbits;
            } else {
                valid = false;      // the 64-bit form needs the full width
            }
        } else {
            valid = false;
        }
        if (valid) {
            if (invert) imm64 = ~imm64;
            char hb[64];
            snprintf(hb, sizeof hb, "0x%llxULL", (unsigned long long)imm64);
            std::string s;
            if (logical) {
                // ORR and BIC read the destination and combine; BIC is spelled
                // as an AND because the immediate was already inverted above.
                const char* lop = (op != 0) ? "&=" : "|=";
                s = "c->vreg[" + std::to_string(rd) + "][0]" + lop + hb + "; ";
                if (Q) s += "c->vreg[" + std::to_string(rd) + "][1]" + lop + hb + ";";
                else   s += "c->vreg[" + std::to_string(rd) + "][1]=0;";
            } else {
                s = "c->vreg[" + std::to_string(rd) + "][0]=" + hb + "; c->vreg[" +
                    std::to_string(rd) + "][1]=" + (Q ? hb : "0") + ";";
            }
            put(s);
            return true;
        }
    }

    // Advanced SIMD shift by immediate: SSHR/USHR, SHL and the lengthening
    // SSHLL/USHLL. immh is non-zero here and names the element width; the
    // shift amount is measured from that width, upwards for the left shifts
    // and downwards for the right ones.
    // Deslocamento escalar por imediato, so as formas que estreitam com
    // saturacao: SQSHRUN/SQRSHRUN, SQSHRN/SQRSHRN e UQSHRN/UQRSHRN. A conta e
    // a mesma da forma vetorial com uma unica faixa; nao ha metade a escolher,
    // por isso o destino e sempre reescrito por inteiro.
    if ((i & 0xDF000400) == 0x5F000400 && ((i >> 19) & 15) != 0) {
        const u32 U = (i >> 29) & 1;
        const u32 immh = (i >> 19) & 15, immb = (i >> 16) & 7, opcode = (i >> 11) & 0x1F;
        const u32 rn = (i >> 5) & 31, rd = i & 31;
        const u32 size = (immh & 8) ? 3 : (immh & 4) ? 2 : (immh & 2) ? 1 : 0;
        const u32 esize = 8u << size;
        const u32 imm = (immh << 3) | immb;
        const u32 shift = 2 * esize - imm;
        const bool round = (opcode == 0x11 || opcode == 0x13);
        const bool narrow = (opcode == 0x10 || opcode == 0x11 || opcode == 0x12 || opcode == 0x13);
        // 10000/10001 sao SQSHRUN/SQRSHRUN e existem so com U=1; 10010/10011
        // sao SQSHRN/SQRSHRN com U=0 e UQSHRN/UQRSHRN com U=1.
        const bool unsigned_result = (opcode == 0x10 || opcode == 0x11);
        const bool shape_ok = narrow && size < 3 && shift >= 1 && shift <= esize &&
                              !(unsigned_result && !U);
        if (shape_ok) {
            const std::string uty = "uint" + std::to_string(esize) + "_t";
            const std::string ity = "int" + std::to_string(esize) + "_t";
            const std::string sty = "uint" + std::to_string(esize * 2) + "_t";
            const std::string ssty = "int" + std::to_string(esize * 2) + "_t";
            const bool src_signed = !U || unsigned_result;
            std::string s = "{ " + sty + " _a; " + uty + " _r; ";
            s += "memcpy(&_a,c->vreg[" + std::to_string(rn) + "]," + std::to_string(esize / 4) + "); ";
            const std::string vt = src_signed ? ssty : sty;
            s += vt + " _v=(" + vt + ")_a; ";
            // Arredondar antes do deslocamento transbordaria a faixa larga;
            // deslocar e somar o bit que saiu da o mesmo resultado.
            if (round)
                s += "_v = (" + vt + ")((_v >> " + std::to_string(shift) + ") + ((_v >> " +
                     std::to_string(shift - 1) + ") & 1)); ";
            else
                s += "_v = (" + vt + ")(_v >> " + std::to_string(shift) + "); ";
            const std::string umax_w = "(" + vt + ")(" + uty + ")~(" + uty + ")0";
            if (unsigned_result) {
                s += "_r = _v < 0 ? (" + uty + ")0 : (_v > " + umax_w + " ? (" + uty + ")~(" +
                     uty + ")0 : (" + uty + ")_v); ";
            } else if (U) {
                s += "_r = _v > " + umax_w + " ? (" + uty + ")~(" + uty + ")0 : (" + uty + ")_v; ";
            } else {
                const std::string smax = "(" + ssty + ")(" + ity + ")((" + uty + ")~(" + uty +
                                         ")0 >> 1)";
                const std::string smin = "(" + ssty + ")(-(" + smax + ") - 1)";
                s += "_r = (" + uty + ")(_v > (" + smax + ") ? (" + smax + ") : (_v < (" + smin +
                     ") ? (" + smin + ") : _v)); ";
            }
            s += "c->vreg[" + std::to_string(rd) + "][0]=0; c->vreg[" + std::to_string(rd) +
                 "][1]=0; memcpy(c->vreg[" + std::to_string(rd) + "],&_r," +
                 std::to_string(esize / 8) + "); }";
            put(s);
            return true;
        }
    }

    if ((i & 0x9F800400) == 0x0F000400 && ((i >> 19) & 15) != 0) {
        const u32 Q = (i >> 30) & 1, U = (i >> 29) & 1;
        const u32 immh = (i >> 19) & 15, immb = (i >> 16) & 7, opcode = (i >> 11) & 0x1F;
        const u32 rn = (i >> 5) & 31, rd = i & 31;
        u32 size = 3;
        if (immh & 8) size = 3; else if (immh & 4) size = 2;
        else if (immh & 2) size = 1; else size = 0;
        const u32 esize = 8u << size;
        const u32 imm = (immh << 3) | immb;
        const std::string uty = "uint" + std::to_string(esize) + "_t";
        const std::string ity = "int" + std::to_string(esize) + "_t";

        if (opcode == 0x14) {
            // SSHLL/USHLL widen: the source lanes are half the destination's
            // width, taken from the low half of Vn when Q is clear and from
            // the high half when it is set.
            const u32 shift = imm - esize;
            const std::string wty = "uint" + std::to_string(esize * 2) + "_t";
            const std::string wid = (U ? "(" + wty + ")" : "(" + wty + ")(int" +
                                     std::to_string(esize * 2) + "_t)(" + ity + ")");
            if (size < 3 && shift < esize) {
                const int lanes = 8 / (esize / 8);
                std::string s = "{ " + uty + " _a[" + std::to_string(lanes) + "]; " + wty +
                                " _r[" + std::to_string(lanes) + "]; ";
                s += "memcpy(_a,(const uint8_t*)c->vreg[" + std::to_string(rn) + "]+" +
                     std::to_string(Q ? 8 : 0) + ",8); ";
                s += "for(int _i=0;_i<" + std::to_string(lanes) + ";_i++) _r[_i]=(" + wty +
                     ")(" + wid + "_a[_i] << " + std::to_string(shift) + "); ";
                s += "c->vreg[" + std::to_string(rd) + "][0]=0; c->vreg[" + std::to_string(rd) +
                     "][1]=0; memcpy(c->vreg[" + std::to_string(rd) + "],_r,16); }";
                put(s);
                return true;
            }
        } else if (opcode == 0x1C || opcode == 0x1F) {
            // Vector conversions with a fixed-point operand: SCVTF/UCVTF at
            // opcode 11100 and FCVTZS/FCVTZU at 11111. The fraction width is
            // measured down from twice the element size, as for the shifts.
            const u32 fbits = 2 * esize - imm;
            const char* ct = esize == 64 ? "double" : "float";
            const int lanes = (Q ? 16 : 8) / (int)(esize / 8);
            if ((esize == 32 || esize == 64) && !(esize == 64 && !Q) &&
                fbits >= 1 && fbits <= esize) {
                char scale_lit[64];
                snprintf(scale_lit, sizeof scale_lit, "%s0x1p%u",
                         esize == 64 ? "" : "(float)", fbits);
                std::string s;
                if (opcode == 0x1C) {
                    const std::string ity2 = (U ? "uint" : "int") + std::to_string(esize) + "_t";
                    s = "{ " + ity2 + " _a[" + std::to_string(lanes) + "]; " + std::string(ct) +
                        " _r[" + std::to_string(lanes) + "]; ";
                    s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "]," +
                         std::to_string(Q ? 16 : 8) + "); ";
                    s += "for(int _i=0;_i<" + std::to_string(lanes) + ";_i++) _r[_i]=(" +
                         std::string(ct) + ")_a[_i] / " + scale_lit + "; ";
                } else {
                    // Same saturating convert as the scalar form: NaN gives
                    // zero and out-of-range clamps rather than being undefined.
                    const std::string oty = (U ? "uint" : "int") + std::to_string(esize) + "_t";
                    const char* lo = U ? "0.0" : (esize == 64 ? "-9223372036854775808.0"
                                                             : "-2147483648.0");
                    const char* hi = U ? (esize == 64 ? "18446744073709551615.0" : "4294967295.0")
                                       : (esize == 64 ? "9223372036854775807.0" : "2147483647.0");
                    const std::string sat_lo = U ? "0" : (esize == 64 ? "(-9223372036854775807LL - 1)"
                                                                      : "(-2147483647 - 1)");
                    const std::string sat_hi = U ? (esize == 64 ? "0xFFFFFFFFFFFFFFFFULL" : "0xFFFFFFFFU")
                                                 : (esize == 64 ? "0x7FFFFFFFFFFFFFFFLL" : "0x7FFFFFFF");
                    s = "{ " + std::string(ct) + " _a[" + std::to_string(lanes) + "]; " + oty +
                        " _r[" + std::to_string(lanes) + "]; ";
                    s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "]," +
                         std::to_string(Q ? 16 : 8) + "); ";
                    s += "for(int _i=0;_i<" + std::to_string(lanes) + ";_i++){ " + std::string(ct) +
                         " _v=_a[_i] * " + scale_lit + "; ";
                    s += "if (_v != _v) _r[_i]=0; ";
                    s += std::string("else if (!(_v > (") + ct + ")" + lo + ")) _r[_i]=(" + oty +
                         ")" + sat_lo + "; ";
                    s += std::string("else if (!(_v < (") + ct + ")" + hi + ")) _r[_i]=(" + oty +
                         ")" + sat_hi + "; ";
                    s += "else _r[_i]=(" + oty + ")_v; } ";
                }
                s += "c->vreg[" + std::to_string(rd) + "][0]=0; c->vreg[" + std::to_string(rd) +
                     "][1]=0; memcpy(c->vreg[" + std::to_string(rd) + "],_r," +
                     std::to_string(Q ? 16 : 8) + "); }";
                put(s);
                return true;
            }
        } else if (opcode == 0x12 || opcode == 0x13) {
            // SQSHRN/SQRSHRN (U=0) e UQSHRN/UQRSHRN (U=1): estreitar com
            // saturacao, mantendo o sinal da fonte. Ao contrario de SQSHRUN
            // logo abaixo, o resultado tem o mesmo sinal que o operando.
            const u32 shift = 2 * esize - imm;
            const std::string sty = "uint" + std::to_string(esize * 2) + "_t";
            const std::string ssty = "int" + std::to_string(esize * 2) + "_t";
            if (size < 3 && shift >= 1 && shift <= esize) {
                const int lanes = 8 / (int)(esize / 8);
                std::string s = "{ " + sty + " _a[" + std::to_string(lanes) + "]; " + uty +
                                " _r[" + std::to_string(lanes) + "]; ";
                s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "],16); ";
                if (U) {
                    const std::string umax = "(" + sty + ")(" + uty + ")~(" + uty + ")0";
                    s += "for(int _i=0;_i<" + std::to_string(lanes) + ";_i++){ " + sty + " _v=_a[_i]; ";
                    // Arredondar antes de deslocar transbordaria a faixa larga;
                    // deslocar e somar o bit que saiu da o mesmo resultado.
                    if (opcode == 0x13)
                        s += "_v = (" + sty + ")((_v >> " + std::to_string(shift) + ") + ((_v >> " +
                             std::to_string(shift - 1) + ") & 1)); ";
                    else
                        s += "_v = (" + sty + ")(_v >> " + std::to_string(shift) + "); ";
                    s += "_r[_i] = _v > " + umax + " ? (" + uty + ")~(" + uty + ")0 : (" + uty +
                         ")_v; } ";
                } else {
                    const std::string smax = "(" + ssty + ")(" + ity + ")((" + uty + ")~(" + uty +
                                             ")0 >> 1)";
                    const std::string smin = "(" + ssty + ")(-(" + smax + ") - 1)";
                    s += "for(int _i=0;_i<" + std::to_string(lanes) + ";_i++){ " + ssty + " _v=(" +
                         ssty + ")_a[_i]; ";
                    if (opcode == 0x13)
                        s += "_v = (" + ssty + ")((_v >> " + std::to_string(shift) + ") + ((_v >> " +
                             std::to_string(shift - 1) + ") & 1)); ";
                    else
                        s += "_v = (" + ssty + ")(_v >> " + std::to_string(shift) + "); ";
                    s += "_r[_i] = (" + uty + ")(_v > (" + smax + ") ? (" + smax + ") : (_v < (" +
                         smin + ") ? (" + smin + ") : _v)); } ";
                }
                if (Q) s += "memcpy((uint8_t*)c->vreg[" + std::to_string(rd) + "]+8,_r,8); }";
                else   s += "c->vreg[" + std::to_string(rd) + "][0]=0; c->vreg[" +
                            std::to_string(rd) + "][1]=0; memcpy(c->vreg[" + std::to_string(rd) +
                            "],_r,8); }";
                put(s);
                return true;
            }
        } else if ((opcode == 0x10 || opcode == 0x11) && U) {
            // SQSHRUN/SQRSHRUN: fonte com sinal, resultado sem sinal, com
            // saturacao nos dois extremos.
            const u32 shift = 2 * esize - imm;
            const std::string sty = "uint" + std::to_string(esize * 2) + "_t";
            const std::string ssty = "int" + std::to_string(esize * 2) + "_t";
            if (size < 3 && shift >= 1 && shift <= esize) {
                const int lanes = 8 / (int)(esize / 8);
                const std::string umax = "(" + uty + ")~(" + uty + ")0";
                std::string s = "{ " + sty + " _a[" + std::to_string(lanes) + "]; " + uty +
                                " _r[" + std::to_string(lanes) + "]; ";
                s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "],16); ";
                s += "for(int _i=0;_i<" + std::to_string(lanes) + ";_i++){ " + ssty + " _v=(" +
                     ssty + ")_a[_i]; ";
                if (opcode == 0x11)
                    s += "_v = (" + ssty + ")((_v >> " + std::to_string(shift) + ") + ((_v >> " +
                         std::to_string(shift - 1) + ") & 1)); ";
                else
                    s += "_v = (" + ssty + ")(_v >> " + std::to_string(shift) + "); ";
                s += "_r[_i] = _v < 0 ? (" + uty + ")0 : (_v > (" + ssty + ")" + umax + " ? " +
                     umax + " : (" + uty + ")_v); } ";
                if (Q) s += "memcpy((uint8_t*)c->vreg[" + std::to_string(rd) + "]+8,_r,8); }";
                else   s += "c->vreg[" + std::to_string(rd) + "][0]=0; c->vreg[" +
                            std::to_string(rd) + "][1]=0; memcpy(c->vreg[" + std::to_string(rd) +
                            "],_r,8); }";
                put(s);
                return true;
            }
        } else if (opcode == 0x10 || opcode == 0x11) {
            // SHRN and RSHRN narrow: the source elements are twice the width
            // immh names, and Q picks which half of Vd takes the result, so
            // the Q=1 form leaves the low half alone. RSHRN rounds by adding
            // half an output unit before the shift.
            const u32 shift = 2 * esize - imm;
            const std::string sty = "uint" + std::to_string(esize * 2) + "_t";
            if (!U && size < 3 && shift >= 1 && shift <= esize) {
                const int lanes = 8 / (int)(esize / 8);
                const std::string round = opcode == 0x11
                    ? ("(" + sty + ")(_a[_i] + ((" + sty + ")1 << " + std::to_string(shift - 1) + "))")
                    : "_a[_i]";
                std::string s = "{ " + sty + " _a[" + std::to_string(lanes) + "]; " + uty +
                                " _r[" + std::to_string(lanes) + "]; ";
                s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "],16); ";
                s += "for(int _i=0;_i<" + std::to_string(lanes) + ";_i++) _r[_i]=(" + uty + ")((" +
                     round + ") >> " + std::to_string(shift) + "); ";
                if (Q) s += "memcpy((uint8_t*)c->vreg[" + std::to_string(rd) + "]+8,_r,8); }";
                else   s += "c->vreg[" + std::to_string(rd) + "][0]=0; c->vreg[" +
                            std::to_string(rd) + "][1]=0; memcpy(c->vreg[" + std::to_string(rd) +
                            "],_r,8); }";
                put(s);
                return true;
            }
        } else if (opcode == 0x00 || opcode == 0x02 || opcode == 0x04 ||
                   opcode == 0x06 || opcode == 0x0A) {
            // SSHR/USHR shift right by (2*esize - imm); SHL shifts left by
            // (imm - esize). SLI shares SHL's opcode under U and is not
            // decoded here.
            const bool left = (opcode == 0x0A);
            const bool accumulate = (opcode == 0x02 || opcode == 0x06);  // S/USRA, S/URSRA
            const bool rounding = (opcode == 0x04 || opcode == 0x06);    // S/URSHR, S/URSRA
            if (!(left && U) && !(size == 3 && !Q)) {
                const u32 shift = left ? (imm - esize) : (2 * esize - imm);
                const int bytes = Q ? 16 : 8;
                const int lanes = bytes / (esize / 8);
                std::string expr;
                if (rounding) {
                    // Somar meia unidade antes de deslocar transborda a faixa
                    // quando o valor esta perto do maximo, e a arquitetura faz
                    // a conta em precisao ilimitada. Deslocar e depois somar o
                    // bit que saiu da o mesmo resultado sem transbordar.
                    const std::string v = U ? "_a[_i]" : ("(" + ity + ")_a[_i]");
                    if (shift == esize) {
                        expr = U ? ("(" + uty + ")(_a[_i] >> " + std::to_string(esize - 1) + ")")
                                 : ("(" + uty + ")0");
                    } else {
                        expr = "(" + uty + ")((" + v + " >> " + std::to_string(shift) + ") + ((" +
                               v + " >> " + std::to_string(shift - 1) + ") & 1))";
                    }
                } else if (left) {
                    expr = "(" + uty + ")(_a[_i] << " + std::to_string(shift) + ")";
                } else if (U) {
                    // A right shift by the whole element width is a legal
                    // encoding whose result is zero; written as a C shift it
                    // would be undefined, so spell the answer out instead.
                    expr = shift == esize ? ("(" + uty + ")0")
                                          : ("(" + uty + ")(_a[_i] >> " + std::to_string(shift) + ")");
                } else {
                    // An arithmetic shift has to go through the signed type;
                    // shifting the unsigned lane would zero-fill. A shift by
                    // the full width leaves only sign bits, which is what a
                    // shift by width-1 already gives.
                    const u32 sh = shift == esize ? esize - 1 : shift;
                    expr = "(" + uty + ")((" + ity + ")_a[_i] >> " + std::to_string(sh) + ")";
                }
                if (shift <= esize && !(left && shift >= esize)) {
                    std::string s = "{ " + uty + " _a[" + std::to_string(lanes) + "],_r[" +
                                    std::to_string(lanes) + "]";
                    if (accumulate) s += ",_d[" + std::to_string(lanes) + "]";
                    s += "; ";
                    s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "]," + std::to_string(bytes) + "); ";
                    if (accumulate)
                        s += "memcpy(_d,c->vreg[" + std::to_string(rd) + "]," + std::to_string(bytes) + "); ";
                    s += "for(int _i=0;_i<" + std::to_string(lanes) + ";_i++) _r[_i]=" +
                         (accumulate ? "(" + uty + ")(_d[_i] + (" + uty + ")(" + expr + "))" : expr) + "; ";
                    s += "c->vreg[" + std::to_string(rd) + "][0]=0; c->vreg[" + std::to_string(rd) +
                         "][1]=0; memcpy(c->vreg[" + std::to_string(rd) + "],_r," +
                         std::to_string(bytes) + "); }";
                    put(s);
                    return true;
                }
            }
        }
    }

    // FMLA and FMLS by indexed element. Like FMUL below, Rm is four bits
    // widened by M and the lane index is split across H and L. The product is
    // fused with the accumulator, so this reads the destination first and goes
    // through fma() rather than writing out a separate multiply and add.
    {
        const bool vec_fmla = (i & 0xBF80B400) == 0x0F801000;
        const bool scl_fmla = (i & 0xFF80B400) == 0x5F801000;
        if (vec_fmla || scl_fmla) {
            const u32 Q = (i >> 30) & 1;
            const bool sub = ((i >> 14) & 1) != 0;     // opcode 0101 is FMLS
            const bool dbl = ((i >> 22) & 1) != 0;
            const u32 rn = (i >> 5) & 31, rd = i & 31;
            const u32 rm = ((i >> 16) & 15) | (((i >> 20) & 1) << 4);
            const u32 H = (i >> 11) & 1, L = (i >> 21) & 1;
            const u32 index = dbl ? H : ((H << 1) | L);
            const char* ct = dbl ? "double" : "float";
            const char* fma = dbl ? "fma" : "fmaf";
            const int fsz = dbl ? 8 : 4;
            const int bytes = scl_fmla ? fsz : (Q ? 16 : 8);
            const int lanes = bytes / fsz;
            if (!(dbl && L)) {   // L must be zero for the 64-bit form
                std::string s = "{ " + std::string(ct) + " _a[" + std::to_string(lanes) +
                                "],_r[" + std::to_string(lanes) + "],_m; ";
                s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "]," + std::to_string(bytes) + "); ";
                s += "memcpy(_r,c->vreg[" + std::to_string(rd) + "]," + std::to_string(bytes) + "); ";
                s += "memcpy(&_m,(const uint8_t*)c->vreg[" + std::to_string(rm) + "]+" +
                     std::to_string(index * fsz) + "," + std::to_string(fsz) + "); ";
                s += "for(int _i=0;_i<" + std::to_string(lanes) + ";_i++) _r[_i]=" +
                     std::string(fma) + "(" + (sub ? "-_a[_i]" : "_a[_i]") + ",_m,_r[_i]); ";
                s += "c->vreg[" + std::to_string(rd) + "][0]=0; c->vreg[" + std::to_string(rd) +
                     "][1]=0; memcpy(c->vreg[" + std::to_string(rd) + "],_r," +
                     std::to_string(bytes) + "); }";
                put(s);
                return true;
            }
        }
    }

    // Integer MUL, MLA and MLS by indexed element. The element width comes
    // from size, the lane index is split across H, L and - for 16-bit elements
    // - M, which then no longer widens Rm.
    // O bit 29 fica fora da mascara: MUL por elemento e U=0, mas MLA e MLS
    // sao U=1. Fixa-lo a zero deixava as duas ultimas fora do decodificador
    // apesar de o corpo as tratar.
    if ((i & 0x9F00B400) == 0x0F008000 || (i & 0x9F00B400) == 0x0F000000 ||
        (i & 0x9F00B400) == 0x0F004000) {
        const u32 Q = (i >> 30) & 1, U = (i >> 29) & 1;
        const u32 size = (i >> 22) & 3, opcode = (i >> 12) & 15;
        const u32 H = (i >> 11) & 1, L = (i >> 21) & 1, M = (i >> 20) & 1;
        const u32 rn = (i >> 5) & 31, rd = i & 31;
        const bool is_mul = (opcode == 8), is_mla = (opcode == 0), is_mls = (opcode == 4);
        // MUL has no U=1 form here, and MLA/MLS under U=1 are the same
        // operation, so only the opcode matters for the arithmetic.
        // MUL existe so com U=0; MLA e MLS so com U=1.
        if ((is_mul ? !U : ((is_mla || is_mls) && U)) && (size == 1 || size == 2)) {
            const u32 esize = 8u << size;
            const u32 rm = (size == 1) ? ((i >> 16) & 15) : (((i >> 16) & 15) | (M << 4));
            const u32 index = (size == 1) ? ((H << 2) | (L << 1) | M) : ((H << 1) | L);
            const int bytes = Q ? 16 : 8;
            const int lanes = bytes / (int)(esize / 8);
            const std::string uty = "uint" + std::to_string(esize) + "_t";
            std::string s = "{ " + uty + " _a[" + std::to_string(lanes) + "],_r[" +
                            std::to_string(lanes) + "],_m; ";
            if (!is_mul) s += uty + " _d[" + std::to_string(lanes) + "]; ";
            s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "]," + std::to_string(bytes) + "); ";
            if (!is_mul) s += "memcpy(_d,c->vreg[" + std::to_string(rd) + "]," +
                              std::to_string(bytes) + "); ";
            s += "memcpy(&_m,(const uint8_t*)c->vreg[" + std::to_string(rm) + "]+" +
                 std::to_string(index * (esize / 8)) + "," + std::to_string(esize / 8) + "); ";
            // The product is formed at 64 bits: two 16-bit lanes promote to
            // int, where their product overflows, which is undefined.
            const std::string prod = "(" + uty + ")((uint64_t)_a[_i] * (uint64_t)_m)";
            s += "for(int _i=0;_i<" + std::to_string(lanes) + ";_i++) _r[_i]=" +
                 (is_mul ? prod
                         : ("(" + uty + ")(_d[_i] " + std::string(is_mls ? "-" : "+") + " " +
                            prod + ")")) + "; ";
            s += "memcpy(c->vreg[" + std::to_string(rd) + "],_r," + std::to_string(bytes) + "); ";
            if (!Q) s += "c->vreg[" + std::to_string(rd) + "][1]=0; ";
            s += "}";
            put(s);
            return true;
        }
    }

    // SMLAL/UMLAL, SMLSL/UMLSL e SMULL/UMULL por elemento indexado: como as
    // formas de tres registos diferentes, mas o segundo operando e uma faixa
    // repetida de Rm. Bit 14 distingue acumular de subtrair, por isso fica
    // fora da mascara, exatamente como em MLA/MLS acima.
    if ((i & 0x9F00B400) == 0x0F002000 || (i & 0x9F00B400) == 0x0F00A000) {
        const u32 Q = (i >> 30) & 1, U = (i >> 29) & 1;
        const u32 size = (i >> 22) & 3, opcode = (i >> 12) & 15;
        const u32 H = (i >> 11) & 1, L = (i >> 21) & 1, M = (i >> 20) & 1;
        const u32 rn = (i >> 5) & 31, rd = i & 31;
        const bool wmul = (opcode == 0xA);                    // SMULL/UMULL
        const bool wsub = (opcode == 0x6);                    // SMLSL/UMLSL
        const bool known = (opcode == 0x2 || opcode == 0x6 || wmul);
        if (known && (size == 1 || size == 2)) {
            const u32 esize = 8u << size;
            const u32 rm = (size == 1) ? ((i >> 16) & 15) : (((i >> 16) & 15) | (M << 4));
            const u32 index = (size == 1) ? ((H << 2) | (L << 1) | M) : ((H << 1) | L);
            const int lanes = 8 / (int)(esize / 8);
            const std::string sty = "uint" + std::to_string(esize) + "_t";
            const std::string wty = "uint" + std::to_string(esize * 2) + "_t";
            const std::string wid = U ? ("(" + wty + ")")
                                      : ("(" + wty + ")(int" + std::to_string(esize * 2) +
                                         "_t)(int" + std::to_string(esize) + "_t)");
            std::string s = "{ " + sty + " _a[" + std::to_string(lanes) + "],_m; " + wty + " _r[" +
                            std::to_string(lanes) + "]; ";
            // Q escolhe a metade do operando estreito, como nas formas de tres
            // registos diferentes.
            s += "memcpy(_a,(const uint8_t*)c->vreg[" + std::to_string(rn) + "]+" +
                 std::to_string(Q ? 8 : 0) + ",8); ";
            s += "memcpy(&_m,(const uint8_t*)c->vreg[" + std::to_string(rm) + "]+" +
                 std::to_string(index * (esize / 8)) + "," + std::to_string(esize / 8) + "); ";
            if (!wmul) s += "memcpy(_r,c->vreg[" + std::to_string(rd) + "],16); ";
            // O produto e formado a 64 bits: duas faixas de 16 promovem a int,
            // onde o produto transborda, e isso e indefinido em vez de ciclico.
            const std::string prod = "(" + wty + ")((uint64_t)(" + wid + "_a[_i]) * (uint64_t)(" +
                                     wid + "_m))";
            s += "for(int _i=0;_i<" + std::to_string(lanes) + ";_i++) _r[_i]=" +
                 (wmul ? prod
                       : ("(" + wty + ")(_r[_i] " + std::string(wsub ? "-" : "+") + " " + prod + ")")) +
                 "; ";
            s += "memcpy(c->vreg[" + std::to_string(rd) + "],_r,16); }";
            put(s);
            return true;
        }
    }

    // FMUL by indexed element. Rm is only four bits here, extended by M, and
    // the lane index is split across H and L - reading Rm as the usual five
    // bits would silently address the wrong register.
    {
        // Bit 29 (U) selects FMULX, a different operation - keep it out of FMUL.
        const bool vec_idx = (i & 0xBF00F400) == 0x0F009000;
        const bool scl_idx = (i & 0xFF00F400) == 0x5F009000;
        if ((vec_idx || scl_idx) && ((i >> 23) & 1) == 1) {
            const u32 Q = (i >> 30) & 1;
            const bool dbl = ((i >> 22) & 1) != 0;
            const u32 rn = (i >> 5) & 31, rd = i & 31;
            const u32 rm = ((i >> 16) & 15) | (((i >> 20) & 1) << 4);
            const u32 H = (i >> 11) & 1, L = (i >> 21) & 1;
            const u32 index = dbl ? H : ((H << 1) | L);
            const char* ct = dbl ? "double" : "float";
            const int fsz = dbl ? 8 : 4;
            const int bytes = scl_idx ? fsz : (Q ? 16 : 8);
            const int lanes = bytes / fsz;
            if (!(dbl && L)) {   // L must be zero for the 64-bit form
                std::string s = "{ " + std::string(ct) + " _a[" + std::to_string(lanes) +
                                "],_r[" + std::to_string(lanes) + "],_m; ";
                s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "]," + std::to_string(bytes) + "); ";
                s += "memcpy(&_m,(const uint8_t*)c->vreg[" + std::to_string(rm) + "]+" +
                     std::to_string(index * fsz) + "," + std::to_string(fsz) + "); ";
                s += "for(int _i=0;_i<" + std::to_string(lanes) + ";_i++) _r[_i]=_a[_i]*_m; ";
                s += "c->vreg[" + std::to_string(rd) + "][0]=0; c->vreg[" + std::to_string(rd) +
                     "][1]=0; ";
                s += "memcpy(c->vreg[" + std::to_string(rd) + "],_r," + std::to_string(bytes) + "); }";
                put(s);
                return true;
            }
        }
    }

    // Advanced SIMD copy: DUP, INS, SMOV and UMOV. These move single lanes
    // between vector registers and the general registers, which is how any
    // scalar value gets into or out of vector code - so they turn up
    // constantly. imm5 encodes both the element width and the lane index:
    // the position of its lowest set bit gives the width, and the bits above
    // it give the index.
    {
        const bool vector_copy = (i & 0x9FE08400) == 0x0E000400;
        const bool scalar_copy = (i & 0xFFE08400) == 0x5E000400;
        if (vector_copy || scalar_copy) {
            const u32 Q = (i >> 30) & 1, op = (i >> 29) & 1;
            const u32 imm5 = (i >> 16) & 0x1F, imm4 = (i >> 11) & 0xF;
            const u32 rn = (i >> 5) & 31, rd = i & 31;
            int size = -1;
            u32 index = 0;
            if (imm5 & 1)      { size = 0; index = imm5 >> 1; }
            else if (imm5 & 2) { size = 1; index = imm5 >> 2; }
            else if (imm5 & 4) { size = 2; index = imm5 >> 3; }
            else if (imm5 & 8) { size = 3; index = imm5 >> 4; }
            if (size >= 0) {
                const int esz = 1 << size;
                const std::string sn = std::to_string(rn), sd = std::to_string(rd);
                const std::string off = std::to_string(index * esz);
                // Read the source lane into _e as a byte copy; this avoids any
                // assumption about how the 128-bit register is split in two.
                const std::string read_lane =
                    "uint8_t _s[16]; memcpy(_s,c->vreg[" + sn + "],16); uint64_t _e=0; "
                    "memcpy(&_e,_s+" + off + "," + std::to_string(esz) + "); ";

                if (scalar_copy && !op && imm4 == 0) {
                    // DUP (scalar): the named lane alone, rest of the register zeroed.
                    put("{ " + read_lane + "c->vreg[" + sd + "][0]=_e; c->vreg[" + sd +
                        "][1]=0; }");
                    return true;
                }
                if (vector_copy && !op && (imm4 == 0 || imm4 == 1)) {
                    // DUP (element) or DUP (general): every lane takes the value.
                    const int lanes = (Q ? 16 : 8) / esz;
                    std::string s = "{ ";
                    s += (imm4 == 0) ? read_lane
                                     : ("uint64_t _e=" + Xz(rn) + "; ");
                    s += "uint8_t _d[16]; memset(_d,0,16); ";
                    s += "for(int _i=0;_i<" + std::to_string(lanes) + ";_i++) memcpy(_d+_i*" +
                         std::to_string(esz) + ",&_e," + std::to_string(esz) + "); ";
                    s += "memcpy(c->vreg[" + sd + "],_d,16); }";
                    put(s);
                    return true;
                }
                if (vector_copy && !op && (imm4 == 5 || imm4 == 7) && rd != 31) {
                    // SMOV / UMOV: one lane out into a general register.
                    std::string s = "{ " + read_lane;
                    if (imm4 == 5) {
                        // Sign-extend from the element width.
                        const char* st = size == 0 ? "int8_t"
                                       : size == 1 ? "int16_t"
                                                   : "int32_t";
                        s += "uint64_t _r=(uint64_t)(int64_t)(" + std::string(st) + ")_e; ";
                        if (!Q) s += "_r &= 0xFFFFFFFFULL; ";
                        s += "c->x[" + sd + "]=_r; }";
                    } else {
                        s += "c->x[" + sd + "]=_e; }";
                    }
                    put(s);
                    return true;
                }
                if (vector_copy && !op && imm4 == 3) {
                    // INS (general): overwrite one lane, leave the others alone.
                    put("{ uint8_t _d[16]; memcpy(_d,c->vreg[" + sd + "],16); uint64_t _e=" +
                        Xz(rn) + "; memcpy(_d+" + off + ",&_e," + std::to_string(esz) +
                        "); memcpy(c->vreg[" + sd + "],_d,16); }");
                    return true;
                }
                if (vector_copy && op) {
                    // INS (element): imm4 holds the source lane, above the bits
                    // the element width occupies.
                    const u32 src_index = imm4 >> size;
                    put("{ uint8_t _s[16],_d[16]; memcpy(_s,c->vreg[" + sn +
                        "],16); memcpy(_d,c->vreg[" + sd + "],16); memcpy(_d+" + off + ",_s+" +
                        std::to_string(src_index * esz) + "," + std::to_string(esz) +
                        "); memcpy(c->vreg[" + sd + "],_d,16); }");
                    return true;
                }
            }
        }
    }

    // The Advanced SIMD modified-immediate group is decoded further up,
    // where the full cmode table - MOVI, MVNI, ORR, BIC and the vector
    // FMOV - is handled together.

    // Advanced SIMD scalar three-register same, formas inteiras de 64 bits:
    // CMGT/CMHI, CMGE/CMHS, CMTST/CMEQ e ADD/SUB. Operam sobre um unico D.
    if ((i & 0xDF20FC00) == 0x5E208400 || (i & 0xDF20FC00) == 0x5E208C00 ||
        (i & 0xDF20FC00) == 0x5E203400 || (i & 0xDF20FC00) == 0x5E203C00 ||
        (i & 0xDF20FC00) == 0x5E208400) {
        const u32 U = (i >> 29) & 1, size = (i >> 22) & 3, opc5 = (i >> 11) & 0x1F;
        const u32 rm = (i >> 16) & 31, rn = (i >> 5) & 31, rd = i & 31;
        std::string cond;
        if (size == 3) {
            if (opc5 == 0x06)      cond = U ? "_a > _b" : "(int64_t)_a > (int64_t)_b";
            else if (opc5 == 0x07) cond = U ? "_a >= _b" : "(int64_t)_a >= (int64_t)_b";
            else if (opc5 == 0x11) cond = U ? "_a == _b" : "(_a & _b) != 0";
        }
        if (!cond.empty()) {
            put("{ uint64_t _a=c->vreg[" + std::to_string(rn) + "][0], _b=c->vreg[" +
                std::to_string(rm) + "][0]; c->vreg[" + std::to_string(rd) + "][0]=(" + cond +
                ") ? ~(uint64_t)0 : (uint64_t)0; c->vreg[" + std::to_string(rd) + "][1]=0; }");
            return true;
        }
    }

    // Advanced SIMD scalar pairwise: FADDP, FMAXP/FMINP, FMAXNMP/FMINNMP and
    // the integer ADDP. These fold the two lanes of one vector into a scalar,
    // which is how a dot product or a vector sum finishes.
    if ((i & 0xDF3E0C00) == 0x5E300800) {
        const u32 U = (i >> 29) & 1, a = (i >> 23) & 1;
        const bool dbl = ((i >> 22) & 1) != 0;
        const u32 opcode = (i >> 12) & 0x1F;
        const u32 rn = (i >> 5) & 31, rd = i & 31;
        if (!U && opcode == 0x1B && ((i >> 22) & 3) == 3) {
            // ADDP Dd, Vn.2D - the integer form, 64-bit elements only.
            put("{ uint64_t _a[2]; memcpy(_a,c->vreg[" + std::to_string(rn) + "],16); "
                "c->vreg[" + std::to_string(rd) + "][0]=_a[0]+_a[1]; c->vreg[" +
                std::to_string(rd) + "][1]=0; }");
            return true;
        }
        if (U) {
            const char* ct = dbl ? "double" : "float";
            const int fsz = dbl ? 8 : 4;
            std::string expr;
            if (opcode == 0x0D) expr = "_a[0] + _a[1]";
            else if (opcode == 0x0C) expr = a ? (dbl ? "fmin(_a[0],_a[1])" : "fminf(_a[0],_a[1])")
                                              : (dbl ? "fmax(_a[0],_a[1])" : "fmaxf(_a[0],_a[1])");
            else if (opcode == 0x0F) expr = std::string("(_a[0]!=_a[0]||_a[1]!=_a[1]) ? "
                                                        "(_a[0]+_a[1]) : (_a[0]") +
                                            (a ? "<" : ">") + "_a[1]?_a[0]:_a[1])";
            if (!expr.empty()) {
                std::string s = "{ " + std::string(ct) + " _a[2],_r; ";
                s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "]," + std::to_string(fsz * 2) + "); ";
                s += "_r = " + expr + "; ";
                s += "c->vreg[" + std::to_string(rd) + "][0]=0; c->vreg[" + std::to_string(rd) +
                     "][1]=0; memcpy(c->vreg[" + std::to_string(rd) + "],&_r," +
                     std::to_string(fsz) + "); }";
                put(s);
                return true;
            }
        }
    }

    // Advanced SIMD across lanes: ADDV, SADDLV/UADDLV, SMAXV/UMAXV and
    // SMINV/UMINV. These reduce a whole vector to one scalar, which is how
    // vector code gets a horizontal sum or extreme back into a register - the
    // CNT-then-UADDLV population count is the common case.
    if ((i & 0x9F3E0C00) == 0x0E300800) {
        const u32 Q = (i >> 30) & 1, U = (i >> 29) & 1;
        const u32 size = (i >> 22) & 3, opcode = (i >> 12) & 0x1F;
        const u32 rn = (i >> 5) & 31, rd = i & 31;
        const u32 esize = 8u << size;
        const int bytes = Q ? 16 : 8;
        const int lanes = bytes / (1 << size);
        // 64-bit elements have nothing to reduce, and a 32-bit element only
        // gives more than one lane in the full-width form.
        const bool shape_ok = size < 3 && !(size == 2 && !Q);
        const std::string uty = "uint" + std::to_string(esize) + "_t";
        const std::string ity = "int" + std::to_string(esize) + "_t";
        if (shape_ok) {
            std::string acc, init, wide;
            if (opcode == 0x03) {
                // SADDLV/UADDLV accumulate into twice the element width, so
                // the sum of a full vector cannot overflow.
                wide = (U ? "uint" : "int") + std::to_string(esize * 2) + "_t";
                init = "0";
                acc = U ? "_s = (" + wide + ")(_s + _a[_i])"
                        : "_s = (" + wide + ")(_s + (" + ity + ")_a[_i])";
            } else if (opcode == 0x1B) {
                wide = uty;
                init = "0";
                acc = "_s = (" + uty + ")(_s + _a[_i])";
            } else if (opcode == 0x0A || opcode == 0x1A) {
                const bool is_max = (opcode == 0x0A);
                wide = U ? uty : ity;
                init = U ? "_a[0]" : ("(" + ity + ")_a[0]");
                const std::string cur = U ? "_a[_i]" : ("(" + ity + ")_a[_i]");
                acc = std::string("if (") + cur + (is_max ? " > " : " < ") + "_s) _s = " + cur;
            }
            if (!acc.empty()) {
                const int rsz = (opcode == 0x03) ? (int)(esize * 2) / 8 : (int)esize / 8;
                std::string s = "{ " + uty + " _a[" + std::to_string(lanes) + "]; " + wide +
                                " _s; ";
                s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "]," + std::to_string(bytes) + "); ";
                s += "_s = " + init + "; ";
                s += "for(int _i=" + std::string(opcode == 0x0A || opcode == 0x1A ? "1" : "0") +
                     ";_i<" + std::to_string(lanes) + ";_i++) " + acc + "; ";
                s += "c->vreg[" + std::to_string(rd) + "][0]=0; c->vreg[" + std::to_string(rd) +
                     "][1]=0; memcpy(c->vreg[" + std::to_string(rd) + "],&_s," +
                     std::to_string(rsz) + "); }";
                put(s);
                return true;
            }
        }
    }

    // Advanced SIMD three-register same (0x0E/0x4E/0x6E group).
    // Bit pattern: Q U 0 1 1 1 0 size 1 Rm opcode 1 Rn Rd
    // Fixed bits: [28:24]=01110, bit[21]=1, bit[10]=1
    // Handles the most common vector operations needed by games.
    // Element operations use memcpy to stay strictly conforming C.
    if ((i & 0x9F200400) == 0x0E200400) {
        const u32 Q    = (i >> 30) & 1;   // 0=64-bit half-register, 1=128-bit full
        const u32 U    = (i >> 29) & 1;
        const u32 size = (i >> 22) & 3;   // 0=B 1=H 2=S 3=D (integer); sz for FP
        const u32 rm   = (i >> 16) & 31;
        const u32 opc5 = (i >> 11) & 31;  // bits[15:11]
        const u32 rn   = (i >> 5)  & 31;
        const u32 rd   = i & 31;
        const int vbytes = Q ? 16 : 8;
        const int esz   = 1 << size;      // bytes per integer element
        const int nelems = vbytes / esz;

        // Bitwise ops (opcode=3): AND/BIC/ORR/ORN (U=0); EOR/BSL/BIT/BIF (U=1).
        // These operate on the full register; element size encodes which variant.
        if (opc5 == 3) {
            // Build body string; if left empty, fall through to unhandled.
            std::string body;
            const std::string vd0 = "c->vreg[" + std::to_string(rd) + "][0]";
            const std::string vd1 = "c->vreg[" + std::to_string(rd) + "][1]";
            const std::string vn0 = "c->vreg[" + std::to_string(rn) + "][0]";
            const std::string vn1 = "c->vreg[" + std::to_string(rn) + "][1]";
            const std::string vm0 = "c->vreg[" + std::to_string(rm) + "][0]";
            const std::string vm1 = "c->vreg[" + std::to_string(rm) + "][1]";
            if (!U) {
                const char* op0 = nullptr; const char* op1 = nullptr;
                if      (size == 0) { op0="&";  op1="&";  }   // AND
                else if (size == 1) { op0="&~"; op1="&~"; }   // BIC
                else if (size == 2) { op0="|";  op1="|";  }   // ORR
                else                { op0="|~"; op1="|~"; }   // ORN
                body = vd0 + "=" + vn0 + op0 + vm0 + "; " + vd1 + "=" + vn1 + op1 + vm1 + "; ";
            } else {
                if (size == 0) {  // EOR
                    body = vd0 + "=" + vn0 + "^" + vm0 + "; " + vd1 + "=" + vn1 + "^" + vm1 + "; ";
                } else if (size == 1) {  // BSL: Vd = (Vd & Vn) | (~Vd & Vm)
                    body = vd0 + "=(" + vd0 + "&" + vn0 + ")|(~" + vd0 + "&" + vm0 + "); ";
                    body += vd1 + "=(" + vd1 + "&" + vn1 + ")|(~" + vd1 + "&" + vm1 + "); ";
                } else {
                    // BIT (size=2) inserts where Vm has ones, BIF (size=3)
                    // where Vm has zeros; both leave the rest of Vd alone.
                    const std::string sel0 = (size == 2) ? vm0 : ("~" + vm0);
                    const std::string sel1 = (size == 2) ? vm1 : ("~" + vm1);
                    body = vd0 + "=(" + vd0 + "&~(" + sel0 + "))|(" + vn0 + "&(" + sel0 + ")); ";
                    body += vd1 + "=(" + vd1 + "&~(" + sel1 + "))|(" + vn1 + "&(" + sel1 + ")); ";
                }
            }
            if (!body.empty()) {
                std::string s = "{ " + body;
                if (!Q) s += vd1 + "=0; ";
                s += "}";
                put(s);
                return true;
            }
        }

        // Integer ADD (opc5=16, U=0) and SUB (opc5=16, U=1), element-wise.
        if (opc5 == 16) {
            const char* iop = U ? "-" : "+";
            const int b8 = 8 * esz;
            const std::string ty = "uint" + std::to_string(b8) + "_t";
            std::string s = "{ " + ty + " _a[" + std::to_string(nelems) + "],_b[" + std::to_string(nelems) + "],_r[" + std::to_string(nelems) + "]; ";
            s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "]," + std::to_string(vbytes) + "); ";
            s += "memcpy(_b,c->vreg[" + std::to_string(rm) + "]," + std::to_string(vbytes) + "); ";
            s += "for(int _i=0;_i<" + std::to_string(nelems) + ";_i++) _r[_i]=(" + ty + ")(_a[_i]" + iop + "_b[_i]); ";
            s += "memcpy(c->vreg[" + std::to_string(rd) + "],_r," + std::to_string(vbytes) + "); ";
            if (!Q) s += "c->vreg[" + std::to_string(rd) + "][1]=0; ";
            s += "}";
            put(s);
            return true;
        }

        // MUL (opcode 10011, U=0) - element-wise multiply, integer only.
        // U=1 at the same opcode is PMUL (polynomial), which is not the same
        // operation and stays on the fallback.
        if (opc5 == 0x13 && !U && size < 3) {
            const int b8 = 8 * esz;
            const std::string ty = "uint" + std::to_string(b8) + "_t";
            std::string s = "{ " + ty + " _a[" + std::to_string(nelems) + "],_b[" + std::to_string(nelems) + "],_r[" + std::to_string(nelems) + "]; ";
            s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "]," + std::to_string(vbytes) + "); ";
            s += "memcpy(_b,c->vreg[" + std::to_string(rm) + "]," + std::to_string(vbytes) + "); ";
            // Two 16-bit lanes promote to int and their product overflows it,
            // which is undefined rather than a wrap, so widen first.
            s += "for(int _i=0;_i<" + std::to_string(nelems) + ";_i++) _r[_i]=(" + ty + ")((uint64_t)_a[_i]*(uint64_t)_b[_i]); ";
            s += "memcpy(c->vreg[" + std::to_string(rd) + "],_r," + std::to_string(vbytes) + "); ";
            if (!Q) s += "c->vreg[" + std::to_string(rd) + "][1]=0; ";
            s += "}";
            put(s);
            return true;
        }

        // Element-wise integer operations. All of them share one shape - read
        // the lanes, compute, write back, zero the upper half when Q is clear
        // - so the per-opcode part is just the statement that fills _r[_i].
        {
            const std::string uty = "uint" + std::to_string(8 * esz) + "_t";
            const std::string ity = "int" + std::to_string(8 * esz) + "_t";
            const std::string sa = "(" + ity + ")_a[_i]", sb = "(" + ity + ")_b[_i]";
            const std::string ones = "(" + uty + ")~(" + uty + ")0";
            auto cmp = [&](const std::string& cond) {
                return "_r[_i] = (" + cond + ") ? " + ones + " : (" + uty + ")0;";
            };
            auto assign = [&](const std::string& expr) {
                return "_r[_i] = (" + uty + ")(" + expr + ");";
            };
            // The halving and absolute-difference forms need more range than
            // the element has, so they are computed at 64 bits and narrowed.
            const std::string w = "(int64_t)";
            std::string stmt;
            bool need_d = false, allow64 = true;
            switch (opc5) {
            case 0x00:  // SHADD / UHADD
                stmt = assign(U ? "((uint64_t)_a[_i] + _b[_i]) >> 1"
                                : "(" + w + sa + " + " + sb + ") >> 1");
                allow64 = false; break;
            case 0x01: case 0x05: {  // SQADD / UQADD, SQSUB / UQSUB
                // Saturar sem tipo mais largo: com faixas de 64 bits nao ha
                // onde alargar, por isso o transbordo sai dos sinais. Uma soma
                // com sinal transborda quando os dois operandos tem o mesmo
                // sinal e o resultado tem outro; uma subtracao quando os sinais
                // diferem e o resultado nao acompanha o primeiro operando.
                const bool qsub = (opc5 == 0x05);
                const std::string sign = "((" + uty + ")1 << " + std::to_string(8 * esz - 1) + ")";
                const std::string smax = "(" + uty + ")(" + sign + " - 1)";
                if (U) {
                    if (qsub)
                        stmt = "_r[_i] = _a[_i] < _b[_i] ? (" + uty + ")0 : (" + uty +
                               ")(_a[_i] - _b[_i]);";
                    else
                        stmt = "{ " + uty + " _t = (" + uty + ")(_a[_i] + _b[_i]); "
                               "_r[_i] = _t < _a[_i] ? " + ones + " : _t; }";
                } else {
                    stmt = "{ " + uty + " _t = (" + uty + ")(_a[_i] " + (qsub ? "-" : "+") +
                           " _b[_i]); ";
                    stmt += uty + " _ov = (" + uty + ")((" +
                            (qsub ? "(" + uty + ")(_a[_i] ^ _b[_i])"
                                  : "(" + uty + ")~(" + uty + ")(_a[_i] ^ _b[_i])") +
                            ") & (" + uty + ")(_a[_i] ^ _t)); ";
                    stmt += "_r[_i] = (_ov & " + sign + ") ? (" + uty + ")((_a[_i] & " + sign +
                            ") ? " + sign + " : " + smax + ") : _t; }";
                }
                break;
            }
            case 0x02:  // SRHADD / URHADD
                stmt = assign(U ? "((uint64_t)_a[_i] + _b[_i] + 1) >> 1"
                                : "(" + w + sa + " + " + sb + " + 1) >> 1");
                allow64 = false; break;
            case 0x04:  // SHSUB / UHSUB
                stmt = assign(U ? "((uint64_t)_a[_i] - _b[_i]) >> 1"
                                : "(" + w + sa + " - " + sb + ") >> 1");
                allow64 = false; break;
            case 0x06:  // CMGT / CMHI
                stmt = cmp(U ? "_a[_i] > _b[_i]" : sa + " > " + sb); break;
            case 0x07:  // CMGE / CMHS
                stmt = cmp(U ? "_a[_i] >= _b[_i]" : sa + " >= " + sb); break;
            case 0x08: { // SSHL / USHL - the shift is a signed count per lane,
                         // negative meaning a shift to the right.
                const std::string wide = std::to_string(8 * esz);
                stmt = "{ int _s = (int)(int8_t)(uint8_t)(_b[_i] & 0xFF); ";
                stmt += "if (_s >= " + wide + ") _r[_i] = (" + uty + ")0; ";
                stmt += "else if (_s >= 0) _r[_i] = (" + uty + ")(_a[_i] << _s); ";
                stmt += "else if (_s <= -" + wide + ") _r[_i] = " +
                        (U ? "(" + uty + ")0"
                           : "(" + uty + ")(" + sa + " >> " + std::to_string(8 * esz - 1) + ")") + "; ";
                stmt += "else _r[_i] = " +
                        (U ? "(" + uty + ")(_a[_i] >> -_s)"
                           : "(" + uty + ")(" + sa + " >> -_s)") + "; }";
                break;
            }
            case 0x09: { // SQSHL / UQSHL, saturating shift by a register
                // Only the left shifts saturate; a negative count is a plain
                // right shift. The overflow test is written as a shift back
                // rather than a wider multiply, so this stays in C11 without
                // needing a 128-bit type.
                const std::string wide = std::to_string(8 * esz);
                const std::string smax = "(" + ity + ")((" + uty + ")~(" + uty + ")0 >> 1)";
                const std::string smin = "(" + ity + ")(-" + smax + " - 1)";
                const std::string umax = "(" + uty + ")~(" + uty + ")0";
                stmt = "{ int _s = (int)(int8_t)(uint8_t)(_b[_i] & 0xFF); ";
                if (U) {
                    stmt += "if (_s >= " + wide + ") _r[_i] = _a[_i] ? " + umax + " : (" + uty + ")0; ";
                    stmt += "else if (_s > 0) { " + uty + " _t = (" + uty + ")(_a[_i] << _s); "
                            "_r[_i] = ((" + uty + ")(_t >> _s) != _a[_i]) ? " + umax + " : _t; } ";
                    stmt += "else if (_s == 0) _r[_i] = _a[_i]; ";
                    stmt += "else if (_s <= -" + wide + ") _r[_i] = (" + uty + ")0; ";
                    stmt += "else _r[_i] = (" + uty + ")(_a[_i] >> -_s); }";
                } else {
                    stmt += "{ " + ity + " _v = " + sa + "; ";
                    stmt += "if (_s >= " + wide + ") _r[_i] = _v ? (" + uty + ")(_v > 0 ? " + smax +
                            " : " + smin + ") : (" + uty + ")0; ";
                    stmt += "else if (_s > 0) { " + uty + " _t = (" + uty + ")((" + uty + ")_v << _s); "
                            "_r[_i] = ((" + ity + ")_t >> _s) != _v ? (" + uty + ")(_v > 0 ? " + smax +
                            " : " + smin + ") : _t; } ";
                    stmt += "else if (_s == 0) _r[_i] = _a[_i]; ";
                    stmt += "else if (_s <= -" + wide + ") _r[_i] = (" + uty + ")(_v >> " +
                            std::to_string(8 * esz - 1) + "); ";
                    stmt += "else _r[_i] = (" + uty + ")(_v >> -_s); } }";
                }
                break;
            }
            case 0x16: { // SQDMULH / SQRDMULH
                // Multiplicar, dobrar e ficar com a metade alta, com saturacao.
                // O unico valor que satura e (-2^(n-1)) * (-2^(n-1)).
                if (U || size == 0 || size == 3) break;   // so 16 e 32 bits
                const std::string wide = "int64_t";
                const std::string smax = "(" + ity + ")((" + uty + ")~(" + uty + ")0 >> 1)";
                // Dobrar o produto antes de deslocar transborda int64 quando
                // ambos os operandos sao o minimo; deslocar uma casa a menos da
                // exatamente o mesmo resultado sem transbordar.
                stmt = "{ " + wide + " _p = (" + wide + ")" + sa + " * (" + wide + ")" + sb + "; ";
                stmt += wide + " _h = _p >> " + std::to_string(8 * esz - 1) + "; ";
                stmt += "if (_h > (" + wide + ")" + smax + ") _h = (" + wide + ")" + smax + "; ";
                stmt += "_r[_i] = (" + uty + ")_h; }";
                allow64 = false;
                break;
            }
            case 0x0C:  // SMAX / UMAX - no 64-bit element form exists
                stmt = assign(U ? "_a[_i] > _b[_i] ? _a[_i] : _b[_i]"
                                : sa + " > " + sb + " ? _a[_i] : _b[_i]");
                allow64 = false; break;
            case 0x0D:  // SMIN / UMIN
                stmt = assign(U ? "_a[_i] < _b[_i] ? _a[_i] : _b[_i]"
                                : sa + " < " + sb + " ? _a[_i] : _b[_i]");
                allow64 = false; break;
            case 0x0E:  // SABD / UABD
                stmt = assign(U ? "_a[_i] > _b[_i] ? _a[_i] - _b[_i] : _b[_i] - _a[_i]"
                                : sa + " > " + sb + " ? " + w + sa + " - " + sb +
                                  " : " + w + sb + " - " + sa);
                allow64 = false; break;
            case 0x0F:  // SABA / UABA
                need_d = true;
                stmt = assign("_d[_i] + (" + uty + ")(" +
                              (U ? "_a[_i] > _b[_i] ? _a[_i] - _b[_i] : _b[_i] - _a[_i]"
                                 : sa + " > " + sb + " ? " + w + sa + " - " + sb +
                                   " : " + w + sb + " - " + sa) + ")");
                allow64 = false; break;
            case 0x11:  // CMTST (U=0) / CMEQ (U=1)
                stmt = cmp(U ? "_a[_i] == _b[_i]" : "(_a[_i] & _b[_i]) != 0"); break;
            case 0x12:  // MLA / MLS
                need_d = true;
                allow64 = false;
                // The multiply is done at 64 bits and narrowed. Two 16-bit
                // lanes promote to int, where their product overflows - which
                // is undefined, not a wrap - so the operands cannot be left at
                // their promoted width.
                stmt = assign(std::string("_d[_i] ") + (U ? "-" : "+") +
                              " (" + uty + ")((uint64_t)_a[_i] * (uint64_t)_b[_i])");
                break;
            default: break;
            }
            const bool shape_ok = (allow64 || size < 3) && !(size == 3 && !Q);
            if (!stmt.empty() && shape_ok) {
                const std::string n = std::to_string(nelems);
                const std::string vb = std::to_string(vbytes);
                std::string s = "{ " + uty + " _a[" + n + "],_b[" + n + "],_r[" + n + "]";
                if (need_d) s += ",_d[" + n + "]";
                s += "; ";
                s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "]," + vb + "); ";
                s += "memcpy(_b,c->vreg[" + std::to_string(rm) + "]," + vb + "); ";
                if (need_d) s += "memcpy(_d,c->vreg[" + std::to_string(rd) + "]," + vb + "); ";
                s += "for(int _i=0;_i<" + n + ";_i++) " + stmt + " ";
                s += "memcpy(c->vreg[" + std::to_string(rd) + "],_r," + vb + "); ";
                if (!Q) s += "c->vreg[" + std::to_string(rd) + "][1]=0; ";
                s += "}";
                put(s);
                return true;
            }
        }

        // Pairwise integer forms: ADDP, SMAXP/UMAXP and SMINP/UMINP. Each
        // result lane combines an adjacent pair, taken from Rn for the lower
        // half of the result and from Rm for the upper half, so the two source
        // registers are walked as one concatenated vector.
        if (opc5 == 0x14 || opc5 == 0x15 || opc5 == 0x17) {
            const std::string uty = "uint" + std::to_string(8 * esz) + "_t";
            const std::string ity = "int" + std::to_string(8 * esz) + "_t";
            const std::string x = "_c[2*_i]", y = "_c[2*_i+1]";
            const std::string sx = "(" + ity + ")" + x, sy = "(" + ity + ")" + y;
            std::string expr;
            if (opc5 == 0x17) expr = x + " + " + y;
            else if (opc5 == 0x14) expr = U ? (x + " > " + y + " ? " + x + " : " + y)
                                            : (sx + " > " + sy + " ? " + x + " : " + y);
            else                  expr = U ? (x + " < " + y + " ? " + x + " : " + y)
                                            : (sx + " < " + sy + " ? " + x + " : " + y);
            if (!(size == 3 && !Q) && !(opc5 != 0x17 && size == 3)) {
                const std::string n = std::to_string(nelems);
                const std::string n2 = std::to_string(nelems * 2);
                const std::string vb = std::to_string(vbytes);
                std::string s = "{ " + uty + " _c[" + n2 + "],_r[" + n + "]; ";
                s += "memcpy(_c,c->vreg[" + std::to_string(rn) + "]," + vb + "); ";
                s += "memcpy(_c+" + n + ",c->vreg[" + std::to_string(rm) + "]," + vb + "); ";
                s += "for(int _i=0;_i<" + n + ";_i++) _r[_i]=(" + uty + ")(" + expr + "); ";
                s += "memcpy(c->vreg[" + std::to_string(rd) + "],_r," + vb + "); ";
                if (!Q) s += "c->vreg[" + std::to_string(rd) + "][1]=0; ";
                s += "}";
                put(s);
                return true;
            }
        }

        // FP three-register same. Here the two "size" bits mean something
        // different from the integer forms: bit 23 is an opcode-extension bit
        // (it picks FSUB over FADD, FMLS over FMLA) and bit 22 is sz, the
        // element width. Treating bit 23 as part of an element size is what
        // makes FADD look like an unrelated instruction, so the two are split
        // apart explicitly here.
        {
            const u32 a   = (i >> 23) & 1;   // opcode extension, not a size bit
            const bool dbl = ((i >> 22) & 1) != 0;   // sz: 0 = float, 1 = double
            const char* ct = dbl ? "double" : "float";
            const int fsz  = dbl ? 8 : 4;
            const int fne  = vbytes / fsz;
            // Emit an element-wise FP operation, optionally accumulating into rd.
            auto fp_op = [&](const std::string& expr, bool acc) {
                std::string s = "{ ";
                s += std::string(ct) + " _a[" + std::to_string(fne) + "]";
                s += ",_b[" + std::to_string(fne) + "]";
                if (acc) s += ",_d[" + std::to_string(fne) + "]";
                s += ",_r[" + std::to_string(fne) + "]; ";
                s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "]," + std::to_string(vbytes) + "); ";
                s += "memcpy(_b,c->vreg[" + std::to_string(rm) + "]," + std::to_string(vbytes) + "); ";
                if (acc) s += "memcpy(_d,c->vreg[" + std::to_string(rd) + "]," + std::to_string(vbytes) + "); ";
                s += "for(int _i=0;_i<" + std::to_string(fne) + ";_i++) _r[_i]=(" + std::string(ct) + ")(" + expr + "); ";
                s += "memcpy(c->vreg[" + std::to_string(rd) + "],_r," + std::to_string(vbytes) + "); ";
                if (!Q) s += "c->vreg[" + std::to_string(rd) + "][1]=0; ";
                s += "}";
                put(s);
            };
            // opcode 11001: FMLA (a=0) / FMLS (a=1), both U=0. These are
            // fused - the product is not rounded before the accumulate - so
            // they go through fma()/fmaf() like the scalar FMADD family. A
            // written-out "_d + _a*_b" rounds twice and drifts from hardware.
            if (opc5 == 0x19 && !U) {
                const char* fma = dbl ? "fma" : "fmaf";
                fp_op(std::string(fma) + (a ? "(-_a[_i],_b[_i],_d[_i])"
                                             : "(_a[_i],_b[_i],_d[_i])"), true);
                return true;
            }
            // opcode 11010: FADD (a=0) / FSUB (a=1), both U=0.
            if (opc5 == 0x1A && !U) {
                fp_op(a ? "_a[_i]-_b[_i]" : "_a[_i]+_b[_i]", false);
                return true;
            }
            // opcode 11011 with U=1 and a=0 is FMUL. The U=0 encoding at the
            // same opcode is FMULX, which differs on infinity times zero, so
            // it is deliberately left alone rather than aliased to FMUL.
            if (opc5 == 0x1B && U && !a) {
                fp_op("_a[_i]*_b[_i]", false);
                return true;
            }
            // opcode 11111 with U=1 and a=0 is FDIV.
            if (opc5 == 0x1F && U && !a) {
                fp_op("_a[_i]/_b[_i]", false);
                return true;
            }
            // opcode 11111 with U=0: FRECPS (a=0) and FRSQRTS (a=1), the
            // Newton-Raphson steps that refine an FRECPE or FRSQRTE estimate.
            // Both are fused, and both answer with a fixed constant when one
            // operand is infinite and the other zero - a case where the fused
            // multiply would otherwise produce a NaN.
            if (opc5 == 0x1F && !U) {
                const char* fma = dbl ? "fma" : "fmaf";
                const char* two = a ? (dbl ? "3.0" : "3.0f") : (dbl ? "2.0" : "2.0f");
                const std::string step = std::string(fma) + "(-_a[_i],_b[_i]," + two + ")" +
                                         (a ? (dbl ? " / 2.0" : " / 2.0f") : "");
                const std::string special = a ? (dbl ? "1.5" : "1.5f") : (dbl ? "2.0" : "2.0f");
                fp_op("((_a[_i]==0 && isinf(_b[_i])) || (isinf(_a[_i]) && _b[_i]==0)) ? " +
                      special + " : (" + step + ")", false);
                return true;
            }
            // opcode 11000: FMAXNM (a=0) / FMINNM (a=1), U=0. The NM forms
            // return the other operand when one is NaN, which is what
            // fmax/fmin do; FMAX/FMIN at opcode 11110 propagate the NaN.
            if (opc5 == 0x18 && !U) {
                fp_op(a ? (dbl ? "fmin(_a[_i],_b[_i])" : "fminf(_a[_i],_b[_i])")
                        : (dbl ? "fmax(_a[_i],_b[_i])" : "fmaxf(_a[_i],_b[_i])"), false);
                return true;
            }
            // opcode 11110: FMAX (a=0) / FMIN (a=1), U=0.
            if (opc5 == 0x1E && !U) {
                fp_op(a ? "(_a[_i]!=_a[_i]||_b[_i]!=_b[_i]) ? (_a[_i]+_b[_i]) : "
                          "(_a[_i]<_b[_i]?_a[_i]:_b[_i])"
                        : "(_a[_i]!=_a[_i]||_b[_i]!=_b[_i]) ? (_a[_i]+_b[_i]) : "
                          "(_a[_i]>_b[_i]?_a[_i]:_b[_i])", false);
                return true;
            }
            // opcode 11010 with U=1 and a=1 is FABD, the absolute difference.
            // The same opcode is FADD/FSUB under U=0 and FADDP under U=1,a=0,
            // both handled elsewhere, so all three bits have to be checked.
            if (opc5 == 0x1A && U && a) {
                fp_op(dbl ? "fabs(_a[_i]-_b[_i])" : "fabsf(_a[_i]-_b[_i])", false);
                return true;
            }
            // The FP pairwise forms, all under U=1: FADDP at opcode 11010,
            // FMAXNMP/FMINNMP at 11000 and FMAXP/FMINP at 11110. Each result
            // lane folds an adjacent pair, taken from Rn for the lower half of
            // the result and from Rm for the upper, so the two sources are
            // walked as one concatenated vector.
            if (U && (opc5 == 0x1A || opc5 == 0x18 || opc5 == 0x1E) &&
                !(opc5 == 0x1A && a)) {
                std::string expr;
                if (opc5 == 0x1A) expr = "_c[2*_i] + _c[2*_i+1]";
                else if (opc5 == 0x18) expr = a ? (dbl ? "fmin(_c[2*_i],_c[2*_i+1])"
                                                       : "fminf(_c[2*_i],_c[2*_i+1])")
                                                : (dbl ? "fmax(_c[2*_i],_c[2*_i+1])"
                                                       : "fmaxf(_c[2*_i],_c[2*_i+1])");
                else expr = std::string("(_c[2*_i]!=_c[2*_i]||_c[2*_i+1]!=_c[2*_i+1]) ? "
                                        "(_c[2*_i]+_c[2*_i+1]) : (_c[2*_i]") +
                            (a ? "<" : ">") + "_c[2*_i+1]?_c[2*_i]:_c[2*_i+1])";
                const std::string n = std::to_string(fne);
                const std::string n2 = std::to_string(fne * 2);
                std::string s = "{ " + std::string(ct) + " _c[" + n2 + "],_r[" + n + "]; ";
                s += "memcpy(_c,c->vreg[" + std::to_string(rn) + "]," + std::to_string(vbytes) + "); ";
                s += "memcpy(_c+" + n + ",c->vreg[" + std::to_string(rm) + "]," +
                     std::to_string(vbytes) + "); ";
                s += "for(int _i=0;_i<" + n + ";_i++) _r[_i]=(" + std::string(ct) + ")(" +
                     expr + "); ";
                s += "memcpy(c->vreg[" + std::to_string(rd) + "],_r," + std::to_string(vbytes) + "); ";
                if (!Q) s += "c->vreg[" + std::to_string(rd) + "][1]=0; ";
                s += "}";
                put(s);
                return true;
            }

            // opcode 11100: the vector FP compares. A true lane is all-ones in
            // the integer view, so these write the mask type rather than a
            // float and cannot go through fp_op.
            if (opc5 == 0x1C && !(U == 0 && a == 1)) {
                const char* fcmp = !U ? "==" : (a ? ">" : ">=");
                const std::string uty = "uint" + std::to_string(fsz * 8) + "_t";
                const std::string n = std::to_string(fne);
                std::string s = "{ " + std::string(ct) + " _a[" + n + "],_b[" + n + "]; " +
                                uty + " _r[" + n + "]; ";
                s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "]," + std::to_string(vbytes) + "); ";
                s += "memcpy(_b,c->vreg[" + std::to_string(rm) + "]," + std::to_string(vbytes) + "); ";
                s += "for(int _i=0;_i<" + n + ";_i++) _r[_i]=(_a[_i]" + fcmp +
                     "_b[_i]) ? (" + uty + ")~(" + uty + ")0 : (" + uty + ")0; ";
                s += "memcpy(c->vreg[" + std::to_string(rd) + "],_r," + std::to_string(vbytes) + "); ";
                if (!Q) s += "c->vreg[" + std::to_string(rd) + "][1]=0; ";
                s += "}";
                put(s);
                return true;
            }
        }
    }

    // Advanced SIMD load/store single structure, including the replicating
    // LD1R..LD4R. A single-lane load inserts into the destination and leaves
    // the other lanes alone, which is what separates it from the whole-vector
    // forms below; the replicating variants write the whole register instead.
    // Bit 21 is R here, part of the opcode rather than a fixed zero, so it
    // has to stay out of the mask - unlike the multiple-structures group below.
    if ((i & 0xBF9F0000) == 0x0D000000 || (i & 0xBF800000) == 0x0D800000) {
        const u32 Q = (i >> 30) & 1;
        const bool post = ((i >> 23) & 1) != 0;
        const bool is_load = ((i >> 22) & 1) != 0;
        const u32 R = (i >> 21) & 1, rm = (i >> 16) & 31;
        const u32 opcode = (i >> 13) & 7, S = (i >> 12) & 1, size = (i >> 10) & 3;
        const u32 rn = (i >> 5) & 31, rt = i & 31;
        // opcode<0> and R together name how many registers one structure
        // spans; opcode<2:1> names the element width and where the index
        // comes from.
        const u32 selem = (((opcode & 1) << 1) | R) + 1;
        const bool replicate = (opcode >> 1) == 3;
        u32 esize = 0, index = 0;
        bool shape_ok = false;
        if (replicate) {
            esize = 8u << size;
            shape_ok = is_load;              // there is no replicating store
        } else {
            switch (opcode >> 1) {
            case 0: esize = 8;  index = (Q << 3) | (S << 2) | size; shape_ok = true; break;
            case 1: esize = 16; index = (Q << 2) | (S << 1) | (size >> 1);
                    shape_ok = (size & 1) == 0; break;
            case 2:
                if (size == 0)      { esize = 32; index = (Q << 1) | S; shape_ok = true; }
                else if (size == 1) { esize = 64; index = Q; shape_ok = (S == 0); }
                break;
            default: break;
            }
        }
        if (shape_ok) {
            const u32 ebytes = esize / 8;
            const std::string ld = "recomp_load" + std::to_string(esize);
            const std::string st = "recomp_store" + std::to_string(esize);
            const int bytes = Q ? 16 : 8;
            std::string s = "{ uint64_t _p=c->x[" + std::to_string(rn) + "]; ";
            for (u32 k = 0; k < selem; ++k) {
                const u32 reg = (rt + k) & 31;
                const u64 off = (u64)k * ebytes;
                if (replicate) {
                    // Every lane of the destination takes the same element.
                    s += "{ uint64_t _v=" + ld + "(c,_p+" + std::to_string(off) + "); ";
                    s += "uint" + std::to_string(esize) + "_t _e=(uint" + std::to_string(esize) +
                         "_t)_v, _r[" + std::to_string(bytes / (int)ebytes) + "]; ";
                    s += "for(int _i=0;_i<" + std::to_string(bytes / (int)ebytes) + ";_i++) _r[_i]=_e; ";
                    s += "c->vreg[" + std::to_string(reg) + "][0]=0; c->vreg[" + std::to_string(reg) +
                         "][1]=0; memcpy(c->vreg[" + std::to_string(reg) + "],_r," +
                         std::to_string(bytes) + "); } ";
                } else if (is_load) {
                    // An insert: only the indexed lane changes.
                    s += "{ uint64_t _v=" + ld + "(c,_p+" + std::to_string(off) + "); ";
                    s += "memcpy((uint8_t*)c->vreg[" + std::to_string(reg) + "]+" +
                         std::to_string(index * ebytes) + ",&_v," + std::to_string(ebytes) + "); } ";
                } else {
                    s += "{ uint64_t _v=0; memcpy(&_v,(const uint8_t*)c->vreg[" +
                         std::to_string(reg) + "]+" + std::to_string(index * ebytes) + "," +
                         std::to_string(ebytes) + "); " + st + "(c,_p+" + std::to_string(off) +
                         ",_v); } ";
                }
            }
            if (post) {
                s += "c->x[" + std::to_string(rn) + "]=_p+" +
                     (rm == 31 ? std::to_string(selem * ebytes) + "ULL"
                               : "c->x[" + std::to_string(rm) + "]") + "; ";
            }
            s += "}";
            put(s);
            return true;
        }
    }

    // Advanced SIMD load/store multiple structures: LD1..LD4 and ST1..ST4,
    // in both the plain and the post-indexed form. These are how vector code
    // reads and writes arrays of structures - ST2 of a pair of 2S vectors
    // writes x,y,x,y - so the de-interleaving is the whole point and cannot be
    // reduced to a plain 16-byte move.
    if ((i & 0xBFBF0000) == 0x0C000000 || (i & 0xBFA00000) == 0x0C800000) {
        const u32 Q = (i >> 30) & 1;
        const bool post = ((i >> 23) & 1) != 0;
        const bool is_load = ((i >> 22) & 1) != 0;
        const u32 rm = (i >> 16) & 31, opcode = (i >> 12) & 15, size = (i >> 10) & 3;
        const u32 rn = (i >> 5) & 31, rt = i & 31;
        // opcode names how many registers are covered and how far apart the
        // elements of one structure are: rpt registers written in turn, selem
        // elements interleaved within each step.
        u32 rpt = 0, selem = 0;
        switch (opcode) {
        case 0x0: rpt = 1; selem = 4; break;   // LD4 / ST4
        case 0x2: rpt = 4; selem = 1; break;   // LD1 / ST1, four registers
        case 0x4: rpt = 1; selem = 3; break;   // LD3 / ST3
        case 0x6: rpt = 3; selem = 1; break;   // LD1 / ST1, three registers
        case 0x7: rpt = 1; selem = 1; break;   // LD1 / ST1, one register
        case 0x8: rpt = 1; selem = 2; break;   // LD2 / ST2
        case 0xA: rpt = 2; selem = 1; break;   // LD1 / ST1, two registers
        default: break;
        }
        // A 64-bit element in the half-width form leaves one element per
        // register, which the interleaving forms do not encode.
        const bool shape_ok = rpt != 0 && !(size == 3 && !Q && selem != 1);
        if (shape_ok) {
            const u32 ebytes = 1u << size;
            const u32 elements = (Q ? 16u : 8u) / ebytes;
            const u32 nregs = rpt * selem;
            const u32 total = nregs * elements * ebytes;
            const std::string ld = "recomp_load" + std::to_string(ebytes * 8);
            const std::string st = "recomp_store" + std::to_string(ebytes * 8);
            std::string s = "{ uint64_t _p=c->x[" + std::to_string(rn) + "]; ";
            s += "uint8_t _t[" + std::to_string(nregs) + "][16]; ";
            // A load leaves the upper half of a half-width register zero, and
            // a store needs the registers' current contents, so the staging
            // buffer is either cleared or filled up front.
            if (is_load) {
                s += "memset(_t,0,sizeof _t); ";
            } else {
                for (u32 k = 0; k < nregs; ++k)
                    s += "memcpy(_t[" + std::to_string(k) + "],c->vreg[" +
                         std::to_string((rt + k) & 31) + "],16); ";
            }
            s += "uint64_t _o=0; ";
            for (u32 r = 0; r < rpt; ++r) {
                s += "for(int _e=0;_e<" + std::to_string(elements) + ";_e++){ ";
                for (u32 se = 0; se < selem; ++se) {
                    const u32 k = r * selem + se;
                    if (is_load) {
                        s += "{ uint64_t _v=" + ld + "(c,_p+_o); memcpy(_t[" +
                             std::to_string(k) + "]+_e*" + std::to_string(ebytes) + ",&_v," +
                             std::to_string(ebytes) + "); } ";
                    } else {
                        s += "{ uint64_t _v=0; memcpy(&_v,_t[" + std::to_string(k) + "]+_e*" +
                             std::to_string(ebytes) + "," + std::to_string(ebytes) + "); " +
                             st + "(c,_p+_o,_v); } ";
                    }
                    s += "_o+=" + std::to_string(ebytes) + "; ";
                }
                s += "} ";
            }
            if (is_load) {
                for (u32 k = 0; k < nregs; ++k)
                    s += "memcpy(c->vreg[" + std::to_string((rt + k) & 31) + "],_t[" +
                         std::to_string(k) + "],16); ";
            }
            if (post) {
                // Rm of 31 means the immediate form, whose offset is exactly
                // the number of bytes transferred.
                s += "c->x[" + std::to_string(rn) + "]=_p+" +
                     (rm == 31 ? std::to_string(total) + "ULL" : "c->x[" + std::to_string(rm) + "]") +
                     "; ";
            }
            s += "}";
            put(s);
            return true;
        }
    }

    // SIMD/FP load/store pair with V=1 (LDP/STP for S, D, Q registers).
    // Same general format as the integer pair handler, but with SIMD registers.
    // Sizes: opc=00→32-bit(S), opc=01→64-bit(D), opc=10→128-bit(Q).
    if ((i & 0x3E000000) == 0x2C000000) {
        const u32 opc = i >> 30;
        const bool is_load = (i >> 22) & 1;
        const u32 mode = (i >> 23) & 3;    // 1=post, 2=signed-offset, 3=pre
        const u32 rt2 = (i >> 10) & 31, rn = (i >> 5) & 31, rt = i & 31;
        s32 imm7 = (s32)((i >> 15) & 0x7F);
        if (imm7 & 0x40) imm7 |= ~0x7F;
        if ((opc <= 2) && mode >= 1 && mode <= 3) {
            const u32 sz = opc == 0 ? 4 : opc == 1 ? 8 : 16;
            const s64 off = (s64)imm7 * (s64)sz;
            const int bits = sz * 8;
            std::string s = "{ uint64_t _b=c->x[" + std::to_string(rn) + "]; int64_t _o=" + std::to_string((long long)off) + "; ";
            // std::string, not const char*: this gets concatenated with byte
            // offsets below, and as a pointer that silently becomes pointer
            // arithmetic on the literal rather than building an expression.
            const std::string addr = (mode == 1) ? std::string("_b") : std::string("(_b+_o)");
            if (is_load) {
                if (sz <= 8) {
                    s += "{ uint64_t _v=recomp_load" + std::to_string(bits) + "(c," + addr + "); memcpy(&c->vreg[" + std::to_string(rt) + "][0],&_v," + std::to_string(sz) + "); c->vreg[" + std::to_string(rt) + "][1]=0; }";
                    s += "{ uint64_t _v=recomp_load" + std::to_string(bits) + "(c," + addr + "+" + std::to_string(sz) + "); memcpy(&c->vreg[" + std::to_string(rt2) + "][0],&_v," + std::to_string(sz) + "); c->vreg[" + std::to_string(rt2) + "][1]=0; }";
                } else {
                    // 128-bit: two 64-bit loads per register
                    s += "{ c->vreg[" + std::to_string(rt) + "][0]=recomp_load64(c," + addr + "); c->vreg[" + std::to_string(rt) + "][1]=recomp_load64(c," + addr + "+8); }";
                    s += "{ c->vreg[" + std::to_string(rt2) + "][0]=recomp_load64(c," + addr + "+16); c->vreg[" + std::to_string(rt2) + "][1]=recomp_load64(c," + addr + "+24); }";
                }
            } else {
                if (sz <= 8) {
                    s += "{ uint64_t _v=0; memcpy(&_v,&c->vreg[" + std::to_string(rt) + "][0]," + std::to_string(sz) + "); recomp_store" + std::to_string(bits) + "(c," + addr + ",_v); }";
                    s += "{ uint64_t _v=0; memcpy(&_v,&c->vreg[" + std::to_string(rt2) + "][0]," + std::to_string(sz) + "); recomp_store" + std::to_string(bits) + "(c," + addr + "+" + std::to_string(sz) + ",_v); }";
                } else {
                    s += "{ recomp_store64(c," + addr + ",c->vreg[" + std::to_string(rt) + "][0]); recomp_store64(c," + addr + "+8,c->vreg[" + std::to_string(rt) + "][1]); }";
                    s += "{ recomp_store64(c," + addr + "+16,c->vreg[" + std::to_string(rt2) + "][0]); recomp_store64(c," + addr + "+24,c->vreg[" + std::to_string(rt2) + "][1]); }";
                }
            }
            if (mode == 1 || mode == 3) s += "c->x[" + std::to_string(rn) + "]=_b+_o; ";
            s += "}";
            put(s);
            return true;
        }
    }

    // SIMD/FP single-register load and store: the V=1 counterparts of the
    // integer forms handled further up. Access width is a scale value where
    // 0..3 mean 1/2/4/8 bytes and 4 means a whole 16-byte Q register; the
    // 16-byte case is encoded as size==00 with the high bit of opc set, which
    // is why the width cannot simply be read off the size field.
    const auto simd_scale = [](u32 size, u32 opc) {
        return (size == 0 && (opc & 2)) ? 4 : (int)size;
    };
    const auto simd_mem = [&](u32 rt, const std::string& ea, int scale, bool is_load) {
        const int nb = 1 << scale;
        const std::string v0 = "c->vreg[" + std::to_string(rt) + "][0]";
        const std::string v1 = "c->vreg[" + std::to_string(rt) + "][1]";
        if (is_load) {
            if (nb == 16) {
                return v0 + "=recomp_load64(c," + ea + "); " + v1 + "=recomp_load64(c,(" + ea + ")+8); ";
            }
            // Narrower loads zero the rest of the register, as the architecture
            // requires - the destination is written whole, not merged into.
            return "{ uint64_t _v=recomp_load" + std::to_string(nb * 8) + "(c," + ea + "); " +
                   v0 + "=_v; " + v1 + "=0; } ";
        }
        if (nb == 16) {
            return "recomp_store64(c," + ea + "," + v0 + "); recomp_store64(c,(" + ea + ")+8," + v1 + "); ";
        }
        return "{ uint64_t _v=0; memcpy(&_v,&" + v0 + "," + std::to_string(nb) + "); recomp_store" +
               std::to_string(nb * 8) + "(c," + ea + ",_v); } ";
    };

    // LDR/STR (SIMD, register offset): [Xn, Xm{, extend {amount}}].
    if ((i & 0x3F200C00) == 0x3C200800) {
        const u32 size = i >> 30, opc = (i >> 22) & 3;
        const u32 rn = (i >> 5) & 31, rt = i & 31;
        const u32 option = (i >> 13) & 7, S = (i >> 12) & 1;
        const int scale = simd_scale(size, opc);
        // Index register 31 is XZR, not SP.
        const std::string rm_str = Xz((i >> 16) & 31);
        std::string idx;
        switch (option) {
        case 2: idx = "(uint64_t)(uint32_t)" + rm_str; break;                      // UXTW
        case 3: idx = rm_str; break;                                                // LSL/UXTX
        case 6: idx = "(uint64_t)(int64_t)(int32_t)(uint32_t)" + rm_str; break;    // SXTW
        case 7: idx = rm_str; break;                                                // SXTX
        default: break;
        }
        if (!idx.empty()) {
            // The shift amount, when enabled, is the access scale - not the
            // size field, which disagrees with it for the 16-byte form.
            if (S && scale) idx = "((" + idx + ")<<" + std::to_string(scale) + ")";
            const std::string ea = "(c->x[" + std::to_string(rn) + "]+" + idx + ")";
            put("{ " + simd_mem(rt, ea, scale, (opc & 1) != 0) + "}");
            return true;
        }
    }

    // LDR/STR (SIMD, unsigned 12-bit immediate offset) - the commonest form.
    if ((i & 0x3F000000) == 0x3D000000) {
        const u32 size = i >> 30, opc = (i >> 22) & 3;
        const u32 imm12 = (i >> 10) & 0xFFF, rn = (i >> 5) & 31, rt = i & 31;
        const int scale = simd_scale(size, opc);
        const std::string ea = "(c->x[" + std::to_string(rn) + "]+" +
                               std::to_string((unsigned long long)imm12 << scale) + "ULL)";
        put("{ " + simd_mem(rt, ea, scale, (opc & 1) != 0) + "}");
        return true;
    }

    // LDUR/STUR (SIMD) and the pre/post-indexed immediate forms.
    if ((i & 0x3F000000) == 0x3C000000 && ((i >> 24) & 1) == 0) {
        const u32 size = i >> 30, opc = (i >> 22) & 3, mode = (i >> 10) & 3;
        const u32 rn = (i >> 5) & 31, rt = i & 31;
        s32 imm9 = (s32)((i >> 12) & 0x1FF);
        if (imm9 & 0x100) imm9 |= ~0x1FF;
        if (mode == 0 || mode == 1 || mode == 3) {
            const int scale = simd_scale(size, opc);
            std::string s = "{ uint64_t _b=c->x[" + std::to_string(rn) + "]; int64_t _o=" +
                            std::to_string((long long)imm9) + "; ";
            // Post-index accesses the unmodified base; the other two add the
            // offset first. Pre- and post-index both write the base back.
            const std::string ea = (mode == 1) ? "_b" : "(_b+_o)";
            s += simd_mem(rt, ea, scale, (opc & 1) != 0);
            if (mode == 1 || mode == 3) s += "c->x[" + std::to_string(rn) + "]=_b+_o; ";
            s += "}";
            put(s);
            return true;
        }
    }

    // An instruction the decoder doesn't know is reported and stepped over
    // rather than ending the block. recomp_unhandled is a non-fatal stub, and
    // ending the block here would set pc to the following instruction - an
    // address that block discovery never marked as a block start, so the
    // dispatcher could not resolve it and execution would stop dead at the
    // first unimplemented opcode instead of continuing past it.
    // Leaving the block the moment the handler halts is what makes the
    // hand-off to the fallback engine correct: the remaining instructions in
    // this block must not run twice, since the fallback resumes at this same
    // PC and will execute them itself.
    snprintf(buf, sizeof buf,
             "recomp_unhandled(c,0x%08xU,g_module_base+0x%llxULL); if(c->halted) return;", i,
             (unsigned long long)pc);
    put(buf); return true;
}

// Translate() with an optional patch at this instruction. `open` follows
// Translate's return value: false only when every path through the emitted code
// ends the block.
inline void TranslatePatched(u32 word, u64 pc, const Patch* patch, std::string& out, bool& open) {
    if (!patch) {
        open = Translate(word, pc, out);
        return;
    }
    if (word != patch->expected) {
        char message[128];
        snprintf(message, sizeof message, "patch at 0x%llx expects 0x%08x, found 0x%08x",
                 (unsigned long long)pc, patch->expected, word);
        throw std::runtime_error(message);
    }
    if (patch->kind == Patch::Kind::Hook) {
        out += "    { extern int " + patch->hook + "(GuestContext*);\n"
               "      if (acre_fps60 && " + patch->hook + "(c)) return; }\n";
        open = Translate(word, pc, out);
        return;
    }
    std::string replaced, original;
    const bool open_replaced = Translate(patch->replacement, pc, replaced);
    const bool open_original = Translate(word, pc, original);
    out += "    if (acre_fps60) {\n" + replaced + "    } else {\n" + original + "    }\n";
    open = open_replaced || open_original;
}

inline std::string FuncName(const std::string& mod, u64 v) {
    char b[64]; snprintf(b, sizeof b, "blk_%s_%016llx", mod.c_str(), (unsigned long long)v); return b;
}

// Same name, written into a caller-owned buffer. The emit loops call this once
// per block for the body and twice more per block for the dispatch table, so on
// a multi-million-block title the returned-std::string form above is millions of
// heap allocations for a name that is always well under 64 bytes.
inline size_t FuncNameTo(char (&b)[64], const char* mod, u64 v) {
    return (size_t)snprintf(b, sizeof b, "blk_%s_%016llx", mod, (unsigned long long)v);
}

const char* RuntimeH();
const char* RuntimeC();

// Stats returned to the caller for manifest/reporting.
struct RecompileStats {
    size_t blocks = 0;
    size_t instructions = 0;
    size_t translated_terminators = 0;
};

// Emit a buildable C project that statically recompiles `text` (raw AArch64 .text at `base`).
// Layout written into out_dir:
//   CMakeLists.txt, main.c, recomp_export.c, recomp_runtime.{c,h}   <- hand-readable top level
//   src/recompiled_<mod>.c, src/recompiled_<mod>_<n>.c              <- generated translation units
//   data/{text,rodata,data}.bin                                     <- bundled guest segments
//   build/                                                          <- cmake's one canonical build dir
// The translation units are kept in src/ rather than the module root: a large
// title emits hundreds of them, and loose in the root they bury the four files
// anyone actually needs to look at.
// Optional rodata/data parameters bundle those segments so the exported exe is self-contained.
// Liga um bloco diretamente ao seu sucessor quando o sucessor e conhecido.
//
// Cada bloco terminava a escrever o PC e a voltar ao despachante, que procurava
// o bloco seguinte e o chamava por ponteiro. Medido no jogo, isso e uma ida ao
// despachante a cada 4,3 instrucoes do convidado - o padrao que a nota de
// engenharia chama "interpretador indirecto caro". Das arestas literais do
// main, 86,9% dos blocos so tem sucessores literais e 100% dessas arestas tem
// bloco no destino, e um percurso medio atravessa 6,2 blocos antes de chegar a
// um salto indireto.
//
// Chamar o sucessor em posicao de cauda deixa o compilador emitir um salto
// direto, sem procura e sem chamada indireta. O orcamento e o que impede um
// ciclo do convidado de nunca mais devolver o controlo: sem ele, as
// interrupcoes, os SVC pendentes e o pedido de paragem deixariam de ser vistos.
// O PC e escrito na mesma antes do salto, para que o caminho em que o orcamento
// se esgota deixe o estado exatamente como o despachante espera.
inline std::string ChainSuccessors(const std::string& body, const std::string& mod,
                                   const std::unordered_set<u64>& block_starts) {
    static const std::string prefix = "c->pc=g_module_base+0x";
    if (body.find(prefix) == std::string::npos) return body;
    std::string out;
    out.reserve(body.size() + 64);
    size_t at = 0;
    for (;;) {
        const size_t hit = body.find(prefix, at);
        if (hit == std::string::npos) {
            out.append(body, at, std::string::npos);
            return out;
        }
        size_t end = hit + prefix.size();
        u64 target = 0;
        bool digits = false;
        while (end < body.size() && isxdigit((unsigned char)body[end])) {
            target = target * 16 + (u64)(isdigit((unsigned char)body[end])
                                             ? body[end] - '0'
                                             : (tolower(body[end]) - 'a' + 10));
            ++end;
            digits = true;
        }
        if (!digits || body.compare(end, 4, "ULL;") != 0) {
            out.append(body, at, end - at);
            at = end;
            continue;
        }
        end += 4;   // consumir "ULL;"
        // Encadear so quando a escrita do PC e mesmo a ultima coisa que o bloco
        // faz. Nao e sempre: o terminador de um SVC escreve o PC e *depois*
        // marca a chamada pendente e chama recomp_svc. Encadear a seguir ao PC
        // saltava por cima da chamada de sistema e o convidado seguia como se
        // ela tivesse acontecido - o jogo deixava de chegar ao fim do arranque e
        // ficava em roda livre. Depois de "ULL;" so pode vir "return;" ou o "}"
        // do ramo condicional.
        {
            size_t after = end;
            while (after < body.size() && (body[after] == ' ' || body[after] == '\n' ||
                                           body[after] == '\t')) {
                ++after;
            }
            const bool terminal = body.compare(after, 7, "return;") == 0 ||
                                  (after < body.size() && body[after] == '}');
            if (!terminal) {
                out.append(body, at, end - at);
                at = end;
                continue;
            }
        }
        if (block_starts.find(target) == block_starts.end()) {
            out.append(body, at, end - at);
            at = end;
            continue;
        }
        out.append(body, at, end - at);
        // Declarada no proprio bloco em vez de no topo da unidade: uma unidade
        // tem vinte mil blocos e os sucessores vao para a frente e para tras,
        // portanto declara-los todos a cabeca custava centenas de milhares de
        // linhas por ficheiro sem ganhar nada.
        const std::string callee = FuncName(mod, target);
        out += " if(--c->chain_budget>0){ void " + callee + "(GuestContext*); " + callee +
               "(c); return; } return;";
        at = end;
    }
}

inline RecompileStats EmitProject(const std::string& mod, const u8* text, size_t n_bytes, u64 base,
                                  const std::string& out_dir, bool source_only,
                                  const u8* rodata = nullptr, size_t rodata_size = 0,
                                  const u8* data_seg = nullptr, size_t data_size = 0,
                                  // Guest address to begin executing at. Defaults to `base`, but a
                                  // real NSO starts with a MOD0 header rather than code, so the
                                  // loader passes the module's actual entry here.
                                  u64 entry_pc = 0,
                                  // Human-readable game name, shown by the generated executable so
                                  // it identifies itself rather than printing raw addresses.
                                  const std::string& display_title = {},
                                  // Addresses of this module's exported dynsym symbols, so
                                  // block discovery seeds a root at each even when nothing in
                                  // this module's own .text branches there directly.
                                  const std::vector<u64>* extra_roots = nullptr,
                                  // Game patches, each applied when lifting the instruction
                                  // at its pc (see TranslatePatched).
                                  const std::vector<Patch>* patches = nullptr) {
    RecompileStats stats;
    auto blocks = DiscoverBlocks(text, n_bytes, base, entry_pc, extra_roots);
    const u32* p = reinterpret_cast<const u32*>(text);
    std::unordered_map<u64, const Patch*> patch_at;
    if (patches) {
        for (const auto& patch : *patches) {
            if (patch.pc < base || patch.pc - base >= n_bytes) {
                throw std::runtime_error("patch outside .text");
            }
            patch_at.emplace(patch.pc, &patch);
        }
    }
    stats.blocks = blocks.size();
    stats.instructions = n_bytes / 4;

    // Block bodies are split across several translation units. A whole game is
    // millions of blocks, which as one .c file runs to hundreds of megabytes -
    // enough that a compiler needs hours and a great deal of memory, or gives
    // up outright. Splitting keeps each unit to a sane size and lets the build
    // compile them in parallel.
    constexpr size_t kBlocksPerUnit = 20000;
    const size_t unit_count =
        blocks.empty() ? 1 : (blocks.size() + kBlocksPerUnit - 1) / kBlocksPerUnit;
    // Written straight to disk rather than buffered. A whole module is over a
    // gigabyte of C, and holding every unit in memory before writing meant the
    // exporter needed that much RAM on top of the game's own data - enough that
    // exporting a large title died partway, after the biggest module and before
    // the rest, leaving an incomplete and inconsistent set of images behind.
    const auto make_dir = [](const std::string& dir) {
        std::error_code ec;
        std::filesystem::create_directories(Utf8Path(dir), ec);
    };
    make_dir(out_dir);
    // Every generated translation unit lands here, keeping the module root down
    // to the few files a human reads.
    const std::string src_dir = out_dir + "/src";
    make_dir(src_dir);
    std::vector<std::ofstream> units;
    units.reserve(unit_count);
    for (size_t u = 0; u < unit_count; ++u) {
        units.emplace_back(Utf8Path(src_dir + "/recompiled_" + mod + "_" + std::to_string(u) + ".c"),
                           std::ios::binary);
        units.back() << "/* auto-generated by suyu static recompiler - DO NOT EDIT */\n"
                        "#include \"recomp_runtime.h\"\n"
                        "#include <stdint.h>\n"
                        "#include <string.h>\n"   // memcpy, used by the scalar FP paths
                        "#include <math.h>\n"
                        "struct _recomp_ent{uint64_t va; BlockFn fn;};\n\n";
    }

    // Each unit accumulates into a string and is written out in large blocks.
    // Streaming every fragment through operator<< meant a sentry construction
    // and a virtual xsputn per token (several per instruction) straight into a
    // default 4 KiB filebuf; batching turns the whole emit into a handful of
    // multi-megabyte writes per unit at no cost in output bytes.
    // Blocks are visited in unit order, so a single buffer serves every unit -
    // one buffer per unit would cost hundreds of megabytes of slack on a title
    // that emits hundreds of units.
    constexpr size_t kUnitFlushBytes = 8u << 20;
    std::string rcu;
    rcu.reserve(kUnitFlushBytes + (1u << 20));
    const auto flush_unit = [&](size_t u, bool force) {
        if (rcu.size() >= kUnitFlushBytes || (force && !rcu.empty())) {
            units[u].write(rcu.data(), (std::streamsize)rcu.size());
            rcu.clear();
        }
    };

    // Every block's own start address, so a terminator can tell whether its
    // successor has code to jump to or has to go back through the dispatcher.
    std::unordered_set<u64> block_starts;
    block_starts.reserve(blocks.size() * 2);
    for (const auto& b : blocks) block_starts.insert(b.vaddr);

    size_t block_index = 0;
    // Hoisted out of the per-instruction loop: a fresh std::string per guest
    // instruction is one allocation per instruction across the whole title.
    std::string body;
    body.reserve(4096);
    char namebuf[64];

    // Each unit carries the dispatch-table entries for its own blocks. Putting
    // the whole table in one file instead meant a single translation unit that
    // grew with the module - hundreds of megabytes of forward declarations and
    // one array initialiser for a large title, compiled by one cl.exe with no
    // parallelism and enormous peak memory, which is both the longest pole in
    // the build and a plausible way for it to die outright. Here the entries
    // sit next to the definitions they point at, so they cost no extra
    // declarations and compile across every core with the bodies.
    std::vector<u64> seg_hi(unit_count, 0);
    const auto emit_seg = [&](size_t u) {
        const size_t lo = u * kBlocksPerUnit;
        size_t hi = lo + kBlocksPerUnit;
        if (hi > blocks.size()) {
            hi = blocks.size();
        }
        char line[160];
        snprintf(line, sizeof line, "\nconst struct _recomp_ent _seg_%s_%zu[] = {\n", mod.c_str(),
                 u);
        rcu += line;
        for (size_t i = lo; i < hi; ++i) {
            FuncNameTo(namebuf, mod.c_str(), blocks[i].vaddr);
            snprintf(line, sizeof line, "  {0x%llxULL, %s},\n",
                     (unsigned long long)blocks[i].vaddr, namebuf);
            rcu += line;
        }
        // C has no empty initialiser list; a zero-length segment still needs a
        // well-formed array, and its declared count of 0 keeps it unsearchable.
        if (lo >= hi) {
            rcu += "  {0ULL, 0}\n";
        } else {
            seg_hi[u] = blocks[hi - 1].vaddr;
        }
        snprintf(line, sizeof line, "};\nconst unsigned _segn_%s_%zu = %zuU;\n", mod.c_str(), u,
                 hi - lo);
        rcu += line;
    };

    size_t cur_unit = 0;
    for (const auto& b : blocks) {
        const size_t unit = block_index / kBlocksPerUnit;
        if (unit != cur_unit) {
            emit_seg(cur_unit);
            flush_unit(cur_unit, true);
            cur_unit = unit;
        }
        ++block_index;
        FuncNameTo(namebuf, mod.c_str(), b.vaddr);
        rcu += "void ";
        rcu += namebuf;
        rcu += "(GuestContext* c){\n";
        const u32 first = (u32)((b.vaddr - base) / 4);
        bool open = true;
        std::string block_body;
        for (u32 k = 0; k < b.count; ++k) {
            body.clear();
            const u64 pc = b.vaddr + (u64)k * 4;
            const auto patched = patch_at.find(pc);
            TranslatePatched(p[first + k], pc, patched == patch_at.end() ? nullptr : patched->second,
                             body, open);
            block_body += body;
            if (!open) { ++stats.translated_terminators; break; }
        }
        // A block that ends by running off its own end still has to hand the
        // dispatcher an absolute guest address, exactly as every branch
        // terminator above does. Emitting the bare module-relative offset here
        // was silently catastrophic: the host dispatcher picks the owning image
        // by "greatest base not exceeding the PC", so a small unbased value has
        // no owner at all and falls through to the "try every image at the raw
        // PC" path - which happily returns *some other module's* block at the
        // same offset and runs it against this module's register state.
        if (open) {
            char tail[80];
            snprintf(tail, sizeof tail, "    c->pc=g_module_base+0x%llxULL; return;\n",
                     (unsigned long long)(b.vaddr + b.size));
            block_body += tail;
        }
        // Guardar os registos do convidado em locais ao longo do bloco foi
        // implementado, testado e medido: as recargas de contexto no assembly
        // caiam de quinze para sete num bloco tipico, e o tempo nao mexia -
        // 3,344/3,373/3,365 s com, 3,307/3,314/3,367 s sem. Reescrever todos os
        // blocos e risco a serio; por zero, nao se paga. Nao repetir.
        rcu += ChainSuccessors(block_body, mod, block_starts);
        rcu += "}\n\n";
        flush_unit(unit, false);
    }
    emit_seg(cur_unit);
    flush_unit(cur_unit, true);

    // The dispatch table is now just a directory of the per-unit segments
    // emitted above: a few lines per unit instead of two entries per block, so
    // this file stays a few kilobytes no matter how large the module is.
    std::string rc;
    rc.reserve(unit_count * 200 + 4096);
    rc += "/* auto-generated by suyu static recompiler - DO NOT EDIT */\n"
          "#include \"recomp_runtime.h\"\n#include <stdint.h>\n\n"
          "struct _recomp_ent{uint64_t va; BlockFn fn;};\n\n";
    {
        char line[192];
        for (size_t u = 0; u < unit_count; ++u) {
            snprintf(line, sizeof line,
                     "extern const struct _recomp_ent _seg_%s_%zu[];\nextern const unsigned "
                     "_segn_%s_%zu;\n",
                     mod.c_str(), u, mod.c_str(), u);
            rc += line;
        }
        rc += "\nstatic const struct _recomp_ent* const _segs[] = {\n";
        for (size_t u = 0; u < unit_count; ++u) {
            snprintf(line, sizeof line, "  _seg_%s_%zu,\n", mod.c_str(), u);
            rc += line;
        }
        rc += "};\n";
        // The per-segment counts and high-water marks the old binary search
        // needed are no longer read by anything; the direct index below finds
        // the segment without comparing addresses at all.
        (void)seg_hi;
    }
    // Direct index from instruction number to block, in place of the two
    // nested binary searches this used to do.
    //
    // Profiling the dispatch loop showed half of all execution time going into
    // finding the block rather than running it - about 36 cycles a lookup, which
    // is what eighteen levels of cache-missing pointer chasing costs. The guest
    // addresses in a module are dense and four-byte aligned, so the instruction
    // number is a perfect index and no search is needed at all. The table holds
    // an encoded segment and offset rather than a function pointer: it keeps the
    // array free of relocations and a quarter of the size, and the second load
    // lands in a segment array small enough to stay cached.
    {
        const size_t n_instr = n_bytes / 4;
        char line[96];
        snprintf(line, sizeof line, "#define _NINSTR %zuU\n", n_instr);
        rc += line;
        rc += "/* 0 means no block starts here; otherwise (segment<<16|offset)+1. */\n";
        snprintf(line, sizeof line, "static const uint32_t _direct[%zuU] = {\n", n_instr ? n_instr : 1);
        rc += line;
        for (size_t i = 0; i < blocks.size(); ++i) {
            const u64 va = blocks[i].vaddr;
            if (va < base) continue;
            const size_t idx = (size_t)((va - base) / 4);
            if (idx >= n_instr) continue;
            const size_t unit = i / kBlocksPerUnit, off = i % kBlocksPerUnit;
            snprintf(line, sizeof line, "[%zuU]=%zuU,", idx, ((unit << 16) | off) + 1);
            rc += line;
            if ((i & 7) == 7) rc += "\n";
        }
        rc += "};\n";
    }
    rc += "#define _NSEG (sizeof(_segs)/sizeof(_segs[0]))\n"
          "BlockFn recomp_lookup(uint64_t pc){\n"
          "  uint64_t i;\n"
          "  uint32_t code;\n"
          "  if(pc & 3u) return 0;\n"
          "  i = pc >> 2;\n"
          "  if(i >= _NINSTR) return 0;\n"
          "  code = _direct[i];\n"
          "  if(!code) return 0;\n"
          "  --code;\n"
          "  return _segs[code >> 16][code & 0xFFFFu].fn;\n}\n";

    const std::string title_str = display_title.empty() ? mod : display_title;
    std::ostringstream mc;
    mc << "#include \"recomp_runtime.h\"\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n";
    mc << "#ifdef HAVE_SDL2\n#include <SDL2/SDL.h>\n#endif\n\n";
    mc << "#define GUEST_MEM_SIZE (256ULL * 1024 * 1024) /* 256 MB */\n\n";
    mc << "static const char* kGameTitle = \"" << title_str << "\";\n\n";
    mc << "#ifdef HAVE_SDL2\n";
    mc << "static SDL_Window*   g_sdl_window   = NULL;\n";
    mc << "static SDL_Renderer* g_sdl_renderer = NULL;\n";
    mc << "static SDL_Texture*  g_sdl_texture  = NULL;\n";
    mc << "static int g_fb_w = 1280, g_fb_h = 720;\n\n";
    mc << "/* Called from the SVC handler when the guest flushes a framebuffer.\n";
    mc << "   fb: CPU-accessible RGBA8 pixels, w x h. */\n";
    mc << "void recomp_sdl_blit(const void* fb, int w, int h) {\n";
    mc << "  if(!g_sdl_renderer) return;\n";
    mc << "  if(w!=g_fb_w || h!=g_fb_h || !g_sdl_texture) {\n";
    mc << "    if(g_sdl_texture) SDL_DestroyTexture(g_sdl_texture);\n";
    mc << "    g_sdl_texture = SDL_CreateTexture(g_sdl_renderer,\n";
    mc << "      SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, w, h);\n";
    mc << "    g_fb_w=w; g_fb_h=h;\n";
    mc << "  }\n";
    mc << "  SDL_UpdateTexture(g_sdl_texture, NULL, fb, w*4);\n";
    mc << "  SDL_RenderClear(g_sdl_renderer);\n";
    mc << "  SDL_RenderCopy(g_sdl_renderer, g_sdl_texture, NULL, NULL);\n";
    mc << "  SDL_RenderPresent(g_sdl_renderer);\n}\n\n";
    mc << "static int sdl_pump_events(void) {\n";
    mc << "  SDL_Event e;\n";
    mc << "  while(SDL_PollEvent(&e)) {\n";
    mc << "    if(e.type==SDL_QUIT) return 0;\n";
    mc << "    if(e.type==SDL_KEYDOWN && e.key.keysym.sym==SDLK_ESCAPE) return 0;\n";
    mc << "  }\n  return 1;\n}\n#endif /* HAVE_SDL2 */\n\n";
    mc << "int main(int argc, char** argv){\n";
    mc << "  printf(\"=== %s ===\\n\", kGameTitle);\n\n";
    mc << "#ifdef HAVE_SDL2\n";
    mc << "  if(SDL_Init(SDL_INIT_VIDEO) == 0) {\n";
    mc << "    g_sdl_window = SDL_CreateWindow(kGameTitle,\n";
    mc << "      SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720, SDL_WINDOW_RESIZABLE);\n";
    mc << "    if(g_sdl_window)\n";
    mc << "      g_sdl_renderer = SDL_CreateRenderer(g_sdl_window, -1, SDL_RENDERER_ACCELERATED);\n";
    mc << "  } else {\n";
    mc << "    fprintf(stderr, \"[recomp] SDL2 init failed: %s (running headless)\\n\", SDL_GetError());\n";
    mc << "  }\n#endif\n\n";
    mc << "  uint8_t* mem = (uint8_t*)calloc(1, (size_t)GUEST_MEM_SIZE);\n";
    mc << "  if(!mem){ fprintf(stderr,\"Failed to allocate guest memory\\n\"); return 1; }\n";
    mc << "  GuestContext c; memset(&c,0,sizeof c);\n";
    mc << "  c.mem=mem; c.mem_size=GUEST_MEM_SIZE;\n";
    mc << "  c.mem_base_vaddr=0x" << std::hex << base << std::dec << "ULL;\n";
    mc << "  c.heap_base=c.mem_base_vaddr+GUEST_MEM_SIZE/2;\n";
    mc << "  c.heap_cur=c.heap_base; c.heap_end=c.mem_base_vaddr+GUEST_MEM_SIZE;\n";
    mc << "  c.x[31]=c.mem_base_vaddr + GUEST_MEM_SIZE - 16; /* SP */\n";
    mc << "  c.pc=0x" << std::hex << (entry_pc ? entry_pc : base) << std::dec << "ULL;\n\n";
    mc << "  recomp_save_init(&c, argv[0]);\n\n";
    mc << "  { char data_dir[512];\n";
    mc << "    snprintf(data_dir,sizeof data_dir,\"%s\",argv[0]);\n";
    mc << "    char* sl=strrchr(data_dir,'\\\\'); if(!sl) sl=strrchr(data_dir,'/'); if(sl) *(sl+1)=0; else data_dir[0]=0;\n";
    mc << "    strncat(data_dir,\"data\",sizeof(data_dir)-strlen(data_dir)-1);\n";
    mc << "    recomp_load_segments(&c,data_dir);\n  }\n\n";
    mc << "  { uint64_t sz=0;\n";
    mc << "    if(recomp_save_exists(&c,\"autosave.bin\")){\n";
    mc << "      recomp_save_read(&c,\"autosave.bin\",c.mem,(uint64_t)GUEST_MEM_SIZE,&sz);\n";
    mc << "      printf(\"[recomp] Restored autosave (%llu bytes)\\n\",(unsigned long long)sz);\n";
    mc << "    }\n  }\n\n";
    mc << "  printf(\"[recomp] Starting at pc=0x%llx\\n\",(unsigned long long)c.pc);\n";
    mc << "  /* Main loop: pump SDL events while the guest runs */\n";
    mc << "#ifdef HAVE_SDL2\n";
    mc << "  while(!c.halted) {\n";
    mc << "    if(!sdl_pump_events()) break;\n";
    mc << "    recomp_run(&c); /* runs until SVC or halt */\n";
    mc << "  }\n";
    mc << "#else\n";
    mc << "  recomp_run(&c);\n";
    mc << "#endif\n\n";
    mc << "  recomp_save_write(&c,\"autosave.bin\",c.mem,(uint64_t)GUEST_MEM_SIZE);\n";
    mc << "  printf(\"[recomp] halted at pc=0x%llx\\n\",(unsigned long long)c.pc);\n";
    mc << "#ifdef HAVE_SDL2\n";
    mc << "  if(g_sdl_texture)  SDL_DestroyTexture(g_sdl_texture);\n";
    mc << "  if(g_sdl_renderer) SDL_DestroyRenderer(g_sdl_renderer);\n";
    mc << "  if(g_sdl_window)   SDL_DestroyWindow(g_sdl_window);\n";
    mc << "  SDL_Quit();\n";
    mc << "#endif\n";
    mc << "  free(mem);\n  return 0;\n}\n";

    std::ostringstream cm;
    // The generated units live in src/ and include "recomp_runtime.h" from the
    // module root, so the root has to be on the include path.
    cm << "cmake_minimum_required(VERSION 3.13)\n"
       << "# When this directory is pulled into a bigger build (suyu-cmd linking the\n"
       << "# modules statically) it must not start a project of its own - it just\n"
       << "# contributes targets. Standalone it is still a complete project.\n"
       << "if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)\n"
       << "  project(suyu_recompiled C)\n"
       << "else()\n"
       << "  enable_language(C)\n"
       << "endif()\n"
       << "set(CMAKE_C_STANDARD 11)\n"
       << "include_directories(${CMAKE_CURRENT_SOURCE_DIR})\n"
       << "# A host project (suyu) may apply C++ flags to every language; this\n"
       << "# directory is plain C, and MSVC rejects /std:c11 together with\n"
       << "# /std:c++20 outright.\n"
       << "get_directory_property(_recomp_opts COMPILE_OPTIONS)\n"
       << "if(_recomp_opts)\n"
       << "  list(FILTER _recomp_opts EXCLUDE REGEX \"std:c\\\\+\\\\+|std=c\\\\+\\\\+|EHsc|permissive\")\n"
       << "  set_directory_properties(PROPERTIES COMPILE_OPTIONS \"${_recomp_opts}\")\n"
       << "endif()\n\n"
       << "# RECOMP_STATIC_ONLY: build just the static library this module\n"
       << "# contributes to a host executable, skipping the portable standalone exe\n"
       << "# and the loadable shared image.\n"
       << "if(NOT RECOMP_STATIC_ONLY)\n"
       << "# Optional: SDL2 window for display output.\n"
       << "# Install SDL2 (e.g. vcpkg install sdl2) to enable the game window.\n"
       << "find_package(SDL2 QUIET)\n"
       << "endif()\n\n"
       << "# Generated translation units, all under src/.\nset(RECOMP_SOURCES\n"
       << "    src/recompiled_" << mod << ".c";
    for (size_t u = 0; u < unit_count; ++u) {
        cm << "\n    src/recompiled_" << mod << "_" << u << ".c";
    }
    cm << ")\n\n"
       << "# Vector lanes are the reason this is -O2 and not -O1.\n"
       << "#\n"
       << "# A vector instruction is emitted as a fixed-trip loop over lanes held\n"
       << "# in locals. GCC does not vectorise that below -O2, so a 4S add came\n"
       << "# out as fifteen instructions - four scalar addss and a round trip\n"
       << "# through the stack - where -O2 emits five, one of them a single\n"
       << "# addps. Scalar and integer code is genuinely indifferent between the\n"
       << "# two levels, which is why -O1 looked free when it was measured on a\n"
       << "# boot: that workload barely touches vector code. A combat scene is\n"
       << "# mostly vector code, and there it is not free at all.\n"
       << "if(MSVC)\n"
       // /MP is what actually decides wall-clock time here. The Visual Studio
       // generator compiles the files of a single project strictly in
       // sequence, so a module with 100+ generated translation units serialises
       // onto one core no matter what --parallel is passed to `cmake --build`.
       // /MP fans them out across every core. Ninja parallelises on its own and
       // warns about /MP, so gate it on the generator, not just on MSVC.
       // The warning suppressions are the noisy ones the translation
       // unavoidably produces (constant conditionals, provably-taken
       // divide/shift paths); silencing them keeps cl.exe from spending real
       // time formatting hundreds of thousands of diagnostics.
       // /we4189 (from the top-level target's inherited warning-as-error
       // set) turns "unused local" into a hard build failure; the codegen
       // legitimately computes and drops _r on some flag-only paths, so
       // downgrade it back to a warning for generated sources specifically.
       << "  set(_recomp_msvc_opts \"/O1\" \"/wd4127\" \"/wd4723\" \"/wd4102\" \"/wd4101\" "
          "\"/wd4189\")\n"
       << "  if(NOT CMAKE_GENERATOR MATCHES \"Ninja\")\n"
       << "    list(APPEND _recomp_msvc_opts \"/MP\")\n"
       << "  endif()\n"
       << "  set_source_files_properties(${RECOMP_SOURCES} PROPERTIES COMPILE_OPTIONS "
          "\"${_recomp_msvc_opts}\")\n"
       << "else()\n"
       << "  set_source_files_properties(${RECOMP_SOURCES} PROPERTIES COMPILE_OPTIONS \"-O2\")\n"
       << "endif()\n\n"
       << "if(NOT RECOMP_STATIC_ONLY)\n"
       << "add_executable(recompiled main.c recomp_runtime.c ${RECOMP_SOURCES})\n"
       // Nothing in this file forward-declares its own generated functions
       // (every blk_<addr> is one straight-line function, by the thousands to
       // the million - there's no reasonable place to put a matching
       // declaration for each), and short single-letter temporaries like _a/_b
       // recur inside nested blocks constantly. Building this target directly
       // is unaffected, but src/CMakeLists.txt turns exactly those warnings
       // (-Werror=missing-declarations, -Werror=shadow) into hard errors for
       // everything under src/ - which this target becomes part of whenever a
       // suyu-cmd-static per-game build add_subdirectory()s it in. Silence
       // them for this generated code specifically rather than for the parent
       // project; disable the two specific warnings rather than a blanket
       // -Wno-error, since GCC lets an explicit -Werror=X placed earlier on
       // the command line (as src/CMakeLists.txt's is here) win over a later
       // bare -Wno-error - only -Wno-<warning> reliably silences it.
       << "if(NOT MSVC)\n"
       << "  target_compile_options(recompiled PRIVATE -Wno-missing-declarations -Wno-shadow -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-parameter)\n"
       << "endif()\n"
       // The translated C uses libm functions directly (sqrtf, fminf, fmaxf,
       // ...) wherever the guest code did FP math - any real title's code
       // pulls these in by the thousands. On Windows they live in the CRT
       // already; everywhere else they need libm explicitly, or the final
       // link fails with "undefined reference to `sqrtf'" and the like.
       << "if(NOT WIN32)\n"
       << "  target_link_libraries(recompiled m)\n"
       << "endif()\n"
       << "if(SDL2_FOUND)\n"
       << "  target_compile_definitions(recompiled PRIVATE HAVE_SDL2)\n"
       << "  target_include_directories(recompiled PRIVATE ${SDL2_INCLUDE_DIRS})\n"
       << "  target_link_libraries(recompiled ${SDL2_LIBRARIES})\n"
       << "  message(STATUS \"SDL2 found — recompiled will open a game window\")\n"
       << "else()\n"
       << "  message(STATUS \"SDL2 not found — running headless (no window)\")\n"
       << "endif()\n"
       << "# Portable C11: Windows->.exe, Linux/FreeBSD/OpenBSD->ELF, macOS->Mach-O\n"
       << "endif()\n\n";

    // A second target builds the same code as a shared library exporting the
    // block lookup. That is what suyu loads to run this image on
    // Core::ArmRecomp: the emulator supplies memory and services through the
    // host bridge, so main.c - which owns a flat buffer and a stub SVC handler
    // - is deliberately left out of this target.
    //
    // Target/output name is per-module so every NSO's DLL builds side by side
    // without colliding: suyu-cmd's loader (src/suyu_cmd/suyu.cpp) looks for
    // recompiled_rtld.dll / recompiled_image.dll (main) / recompiled_sdk.dll /
    // recompiled_subsdkN.dll next to the exe, in NSO load order.
    const std::string dll_target = (mod == "main") ? "recompiled_image" : ("recompiled_" + mod);
    cm << "if(NOT RECOMP_STATIC_ONLY)\n"
       << "add_library(" << dll_target << " SHARED recomp_export.c recomp_runtime.c "
          "${RECOMP_SOURCES})\n"
       << "set_target_properties(" << dll_target << " PROPERTIES C_VISIBILITY_PRESET hidden "
          "OUTPUT_NAME \"" << dll_target << "\")\n"
       << "target_compile_definitions(" << dll_target
       << " PRIVATE SUYU_HOSTED_RECOMP=1 RECOMP_SHARED_MODULE=1)\n"
       // See the identical comment on the `recompiled` target above.
       << "if(NOT MSVC)\n"
       << "  target_compile_options(" << dll_target << " PRIVATE -Wno-missing-declarations -Wno-shadow -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-parameter)\n"
       << "endif()\n"
       // Same libm dependency as the standalone executable above - a shared
       // library is willing to leave symbols like sqrtf undefined at link
       // time, but that only defers the failure to whichever process later
       // dlopen()s it, so link it explicitly instead.
       << "if(NOT WIN32)\n"
       << "  target_link_libraries(" << dll_target << " m)\n"
       << "endif()\n"
       << "endif()\n\n";

    // Static-library variant. Several of these get linked into ONE host
    // executable (the per-game suyu-cmd build), so every symbol a module owns
    // has to be unique. The generated C is written once and renamed at compile
    // time through -D, which keeps the sources identical between the shared and
    // the static shape.
    //
    // recomp_runtime.c is deliberately NOT part of this target: its contents
    // (recomp_svc, the load/store helpers, recomp_cond, ...) are generic and
    // must exist exactly once in the final link. It is built separately, once,
    // as recomp_runtime_shared.
    const std::string static_target = "recomp_static_" + mod;
    cm << "if(NOT TARGET recomp_runtime_shared)\n"
       << "  add_library(recomp_runtime_shared STATIC recomp_runtime.c)\n"
       << "  target_compile_definitions(recomp_runtime_shared PRIVATE SUYU_HOSTED_RECOMP=1 "
          "RECOMP_STATIC_HOST=1)\n"
       // See the identical comment on the `recompiled` target further up.
       << "  if(NOT MSVC)\n"
       << "    target_compile_options(recomp_runtime_shared PRIVATE -Wno-missing-declarations -Wno-shadow -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-parameter)\n"
       << "  endif()\n"
       << "  target_include_directories(recomp_runtime_shared PUBLIC "
          "${CMAKE_CURRENT_SOURCE_DIR})\n"
       << "endif()\n"
       << "add_library(" << static_target << " STATIC recomp_export.c ${RECOMP_SOURCES})\n"
       // See the identical comment on the `recompiled` target further up.
       << "if(NOT MSVC)\n"
       << "  target_compile_options(" << static_target << " PRIVATE -Wno-missing-declarations -Wno-shadow -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-parameter)\n"
       << "endif()\n"
       << "target_include_directories(" << static_target
       << " PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})\n"
       << "target_link_libraries(" << static_target << " PUBLIC recomp_runtime_shared)\n"
       << "target_compile_definitions(" << static_target
       << " PRIVATE SUYU_HOSTED_RECOMP=1 RECOMP_STATIC_MODULE=1"
       << " g_module_base=g_module_base_" << mod
       << " recomp_lookup=recomp_lookup_" << mod
       << " recomp_image_lookup=recomp_image_lookup_" << mod
       << " recomp_image_set_base=recomp_image_set_base_" << mod
       << " recomp_image_entry=recomp_image_entry_" << mod << ")\n";

    std::ostringstream ex;
    ex << "/* auto-generated by suyu static recompiler - DO NOT EDIT */\n"
          "#include \"recomp_runtime.h\"\n\n"
          "/* The one symbol suyu resolves out of a recompiled image. Named\n"
          "   distinctly from the internal recomp_lookup so the export is\n"
          "   unambiguous, and so the internal one can stay hidden. */\n"
          "#ifdef RECOMP_STATIC_MODULE\n"
          "/* Linked straight into the host executable: static-library symbols are\n"
          "   visible to the linker on their own, and the names are already made\n"
          "   unique per module by the -D renames the build applies. */\n"
          "#define RECOMP_API\n"
          "/* The shared runtime is compiled once for the whole link and therefore\n"
          "   cannot own this - each module needs its own load base. */\n"
          "uint64_t g_module_base = 0;\n"
          "#elif defined(_WIN32)\n"
          "#define RECOMP_API __declspec(dllexport)\n"
          "#else\n"
          "#define RECOMP_API __attribute__((visibility(\"default\")))\n"
          "#endif\n\n"
          "RECOMP_API BlockFn recomp_image_lookup(uint64_t pc){ return recomp_lookup(pc - g_module_base); }\n\n"
          "/* Tells this image where its module actually got loaded, so the\n"
          "   addresses it computes are real rather than module-relative. */\n"
          "RECOMP_API void recomp_image_set_base(uint64_t base){ g_module_base = base; }\n\n"
          "/* Reports the entry PC so the loader does not have to be told it\n"
          "   separately or parse the image again. */\n"
       << "RECOMP_API uint64_t recomp_image_entry(void){ return 0x"
       << std::hex << (entry_pc ? entry_pc : base) << std::dec << "ULL; }\n";

    auto write = [&](const std::string& name, const std::string& data) {
        std::ofstream o(Utf8Path(out_dir + "/" + name), std::ios::binary);
        o.write(data.data(), (std::streamsize)data.size());
    };
    std::ostringstream identity;
    identity << "{\n  \"abi_version\": " << Core::RecompAbiVersion
             << ",\n  \"module\": \"" << mod << "\",\n  \"sha256\": [\""
             << Core::RecompSha256({text, n_bytes}) << "\", \""
             << Core::RecompSha256({rodata, rodata_size}) << "\", \""
             << Core::RecompSha256({data_seg, data_size}) << "\"]\n}\n";
    write("recomp_identity.json", identity.str());
    write("recomp_runtime.h", RuntimeH());
    write("recomp_runtime.c", RuntimeC());
    // The dispatch table is generated code like the block bodies, so it belongs
    // with them rather than at the top level.
    write("src/recompiled_" + mod + ".c", rc);
    // The unit files were streamed out as blocks were translated; just make
    // sure everything has reached disk.
    for (auto& u : units) {
        u.flush();
        u.close();
    }
    write("main.c", mc.str());
    write("recomp_export.c", ex.str());
    write("CMakeLists.txt", cm.str());

    // Bundle text/rodata/data as binary blobs so the exe can load them at startup
    {
        std::string data_subdir = out_dir + "/data";
        make_dir(data_subdir);
        // Always write text.bin
        {
            std::ofstream o(Utf8Path(data_subdir + "/text.bin"), std::ios::binary);
            o.write(reinterpret_cast<const char*>(text), (std::streamsize)n_bytes);
        }
        if (rodata && rodata_size > 0) {
            std::ofstream o(Utf8Path(data_subdir + "/rodata.bin"), std::ios::binary);
            o.write(reinterpret_cast<const char*>(rodata), (std::streamsize)rodata_size);
        }
        if (data_seg && data_size > 0) {
            std::ofstream o(Utf8Path(data_subdir + "/data.bin"), std::ios::binary);
            o.write(reinterpret_cast<const char*>(data_seg), (std::streamsize)data_size);
        }
    }

    return stats;
}

inline const char* RuntimeH() {
    return R"RT(#ifndef SUYU_RECOMP_RUNTIME_H
#define SUYU_RECOMP_RUNTIME_H
#include <stdint.h>
#include <stddef.h>   /* offsetof, for the layout assertions below */
#include <string.h>   /* memcpy, para o caminho rapido de memoria abaixo */

/* Supplied by the host when the recompiled image is driven by an emulator
   rather than run standalone. `size` is 1, 2, 4 or 8 bytes. */
typedef struct RecompHostMem {
    void* user;
    uint64_t (*load)(void* user, uint64_t va, uint32_t size);
    void (*store)(void* user, uint64_t va, uint32_t size, uint64_t value);
    int (*exclusive)(void* user, uint32_t instruction);
    /* Optional fast path for ordinary guest accesses. `page_table` is the
       emulator's page table: an array of 32-byte entries whose first field is
       a host pointer with two attribute bits in its low end, exactly as
       Common::PageTable stores it. A non-zero pointer means the page is plain
       memory that can be read and written directly; zero means the page is
       unmapped, debug or rasterizer-cached, and only the emulator knows what
       to do with it. NULL disables the fast path entirely and every access
       goes back out through load/store above. */
    const void* page_table;
    uint64_t address_space_size;   /* 1 << address space bits; 0 also disables */
} RecompHostMem;

typedef struct GuestContext {
    uint64_t x[32]; uint64_t pc; uint8_t n,z,c,v;
    uint8_t* mem; uint64_t mem_size; uint64_t mem_base_vaddr; int halted;
    /* SVC signalling. The emitted code sets this to the instruction's imm
       before calling recomp_svc(). The standalone runtime services the call
       and clears it; when the recompiled code is instead driven by suyu's
       Core::ArmRecomp backend, recomp_svc leaves it set so the emulator can
       see the pending call and dispatch it through the real HLE kernel.
       ~0ULL means "no SVC pending". */
    uint64_t pending_svc;
    /* SIMD/FP register file, 128 bits each held as two 64-bit halves.
       Deliberately placed after pending_svc: Core::ArmRecomp mirrors only the
       prefix of this struct up to that field, so appending here leaves that
       view valid. */
    uint64_t vreg[32][2];
    /* Thread pointer (TPIDR_EL0). Read/written by MRS/MSR; user code uses it
       for thread-local storage. */
    uint64_t tpidr_el0;
    /* Host memory bridge. NULL for the standalone runtime, which owns the flat
       buffer above. When suyu drives this code through Core::ArmRecomp the
       guest's address space belongs to the emulator, not to us, so every
       access has to go back out to Core::Memory instead of into `mem` -
       otherwise the recompiled code and the HLE kernel would be looking at two
       different memories. */
    const struct RecompHostMem* host_mem;
    /* Read-only thread pointer (TPIDRRO_EL0). Distinct from tpidr_el0 above:
       on Horizon the kernel publishes the calling thread's thread-local
       region here, and that region's first 0x100 bytes are the IPC message
       buffer the kernel reads a SendSyncRequest out of. tpidr_el0 in contrast
       belongs to the guest, which points it at its own thread structure.
       Aliasing the two makes the guest write its IPC header at its thread
       struct instead of the TLS region - the kernel then parses an all-zero
       buffer (magic 0 instead of 'SFCI') - and makes the kernel's per-switch
       publish of the TLS address clobber the guest's thread pointer, which
       turns every C++ thread-local access into a near-null read.
       Appended after host_mem so every pinned offset above stays put. */
    uint64_t tpidrro_el0;
    /* Quantos blocos encadeados ainda podem correr antes de devolver o controlo
       ao emulador. Um bloco que termine num sucessor conhecido chama-o
       diretamente em vez de escrever o PC e voltar ao despachante, o que o
       compilador transforma num salto direto. Sem um limite, um ciclo do
       convidado nunca mais devolveria o controlo e as interrupcoes, os SVC
       pendentes e o pedido de paragem deixariam de ser vistos. O emulador
       repoe isto antes de cada entrada. */
    int32_t chain_budget;
    /* Save-data filesystem state */
    char save_dir[512];
    /* Heap break for SVC memory allocation */
    uint64_t heap_base; uint64_t heap_end; uint64_t heap_cur;
    /* IPC command buffer (simplified HLE) */
    uint32_t ipc_cmd[64];
    /* Open file handles for save data (simplified) */
    void* save_handles[16]; int save_handle_count;
} GuestContext;
/* Game patches choose their behaviour at run time from this flag (60fps on/off);
   the host executable defines it. */
extern volatile int acre_fps60;

/* Core::ArmRecomp mirrors the prefix of this struct with its own declaration
   so the emulator can read and write guest state without including this
   header. Nothing enforces that across the two builds, so the offsets are
   pinned here: if a field is inserted rather than appended, this fails loudly
   at generation time instead of silently handing the emulator the wrong
   register file. Keep in sync with GuestContextView in
   core/arm/recomp/arm_recomp.cpp. */
/* A negative array size rather than _Static_assert: the generated project is
   plain C compiled by whatever the user has, and MSVC's /TC mode rejects the
   C11 form outright. This construct is valid in every C dialect. */
typedef char recomp_layout_pc[offsetof(GuestContext, pc) == 256 ? 1 : -1];
typedef char recomp_layout_svc[offsetof(GuestContext, pending_svc) == 304 ? 1 : -1];
typedef char recomp_layout_vreg[offsetof(GuestContext, vreg) == 312 ? 1 : -1];
typedef char recomp_layout_tpidr[offsetof(GuestContext, tpidr_el0) == 824 ? 1 : -1];
typedef char recomp_layout_host[offsetof(GuestContext, host_mem) == 832 ? 1 : -1];
typedef char recomp_layout_tpidrro[offsetof(GuestContext, tpidrro_el0) == 840 ? 1 : -1];
typedef char recomp_layout_chain[offsetof(GuestContext, chain_budget) == 848 ? 1 : -1];

/* Where this module is actually loaded in the guest's address space.
   Every address the static pass bakes in - ADR/ADRP results, branch targets,
   return addresses - is relative to the module, because that is all it can
   know: the loader picks the real base at run time and it differs per run.
   Leaving it zero gives the module-relative behaviour the standalone runtime
   wants; the emulator sets it once at load so computed data pointers land in
   real memory rather than near null. One global per image, since each module
   is its own shared library. */
extern uint64_t g_module_base;

typedef void (*BlockFn)(GuestContext*);
BlockFn recomp_lookup(uint64_t pc); void recomp_run(GuestContext* c);
/* Bandeiras e condicoes, definidas aqui em vez de no runtime.
 *
 * Sao 113 336 e 131 214 sitios de chamada so no main. Enquanto viviam noutra
 * unidade de traducao, cada um custava uma chamada - e, pior, obrigava o
 * compilador a assumir que a funcao podia alterar todo o GuestContext, o que o
 * fazia derramar e recarregar os registos do convidado a volta de cada
 * comparacao. Visiveis aqui, ele ve que so tocam em quatro bytes de bandeiras.
 *
 * O emissor passa sempre `is_sub`, `is64` e `cond` como literais, por isso os
 * ramos e o switch desaparecem por completo na expansao. */
static inline void recomp_set_flags(GuestContext* c,int is_sub,uint64_t a,uint64_t b,uint64_t r,int is64){
  uint64_t m=is64?~0ULL:0xFFFFFFFFULL; r&=m;a&=m;b&=m;
  { uint64_t s=is64?0x8000000000000000ULL:0x80000000ULL;
    c->z=(uint8_t)(r==0); c->n=(uint8_t)((r&s)?1:0);
    if(is_sub){ c->c=(uint8_t)(a>=b); c->v=(uint8_t)((((a^b)&(a^r))&s)?1:0); }
    else { c->c=(uint8_t)(r<a); c->v=(uint8_t)(((~(a^b)&(a^r))&s)?1:0); } }
}
static inline int recomp_cond(GuestContext* c,unsigned cond){
  int n=c->n,z=c->z,cc=c->c,v=c->v,res;
  switch(cond>>1){case 0:res=z;break;case 1:res=cc;break;case 2:res=n;break;case 3:res=v;break;
   case 4:res=cc&&!z;break;case 5:res=(n==v);break;case 6:res=(n==v)&&!z;break;default:res=1;}
  return ((cond&1)&&cond!=15)? !res:res;
}
/* Architectural AddWithCarry: the shared primitive behind ADC/ADCS/SBC/SBCS.
   recomp_set_flags above cannot serve these - its subtract branch hardcodes a
   carry-in of 1 (c = a >= b), which is only right for a plain SUB. Arguments
   are (context, a, b, carry_in, is64, set_flags); the result is already
   narrowed to the operand width. SBC passes ~Rm as b. */
uint64_t recomp_add_with_carry(GuestContext*,uint64_t,uint64_t,int,int,int);
/* FRECPE and FRSQRTE. These are not 1/x and 1/sqrt(x): the architecture
   defines the result through an eight-bit estimate table, so anything computed
   from a division would disagree with both the hardware and the JIT. Both take
   and return the raw bit pattern; is64 picks double over single. */
uint64_t recomp_frecpe(uint64_t,int);
uint64_t recomp_frsqrte(uint64_t,int);
/* Extensao de criptografia. As tabelas vivem no runtime, nao no corpo dos
   blocos: ha centenas de sitios a chamar estas quatro instrucoes e inlina-las
   multiplicaria o codigo gerado por nada. */
void recomp_aese(GuestContext*,unsigned,unsigned);
void recomp_aesd(GuestContext*,unsigned,unsigned);
void recomp_aesmc(GuestContext*,unsigned,unsigned);
void recomp_aesimc(GuestContext*,unsigned,unsigned);
void recomp_sha1_hash(GuestContext*,unsigned,unsigned,unsigned,int);
void recomp_sha1su0(GuestContext*,unsigned,unsigned,unsigned);
void recomp_sha1su1(GuestContext*,unsigned,unsigned);
void recomp_sha1h(GuestContext*,unsigned,unsigned);
void recomp_sha256h(GuestContext*,unsigned,unsigned,unsigned);
void recomp_sha256h2(GuestContext*,unsigned,unsigned,unsigned);
void recomp_sha256su0(GuestContext*,unsigned,unsigned);
void recomp_sha256su1(GuestContext*,unsigned,unsigned,unsigned);
/* High 64 bits of a 64x64 multiply, for SMULH/UMULH. */
uint64_t recomp_smulh(uint64_t,uint64_t);
uint64_t recomp_umulh(uint64_t,uint64_t);
/* Acessos a memoria do convidado. O caminho rapido resolve o endereco pela
   tabela de paginas do emulador sem sair do runtime; o lento trata do resto.
   Forcar a expansao destas nos blocos foi medido e nao ganhou nada - a chamada
   do caminho lento, ainda que fria, obriga na mesma o compilador a recarregar
   o contexto - e custava 65% de tamanho do executavel. O que resolve e manter
   os registos do convidado em locais dentro do bloco, o que tambem foi medido
   e tambem nao rendeu. O que rendeu foi trocar a pesquisa do bloco por
   indexacao direta; ver recomp_lookup. */
uint64_t recomp_load8(GuestContext*,uint64_t); uint64_t recomp_load16(GuestContext*,uint64_t);
uint64_t recomp_load32(GuestContext*,uint64_t); uint64_t recomp_load64(GuestContext*,uint64_t);
void recomp_store8(GuestContext*,uint64_t,uint64_t); void recomp_store16(GuestContext*,uint64_t,uint64_t);
void recomp_store32(GuestContext*,uint64_t,uint64_t); void recomp_store64(GuestContext*,uint64_t,uint64_t);
void recomp_svc(GuestContext*,unsigned); void recomp_unhandled(GuestContext*,uint32_t,uint64_t);
void recomp_barrier(void);
void recomp_exclusive(GuestContext*,uint32_t,uint64_t);
/* halted values. 1 is an ordinary stop; 2 asks the host to re-execute the
   current PC on its interpreter fallback because the decoder had no
   translation for the instruction there. */
#define RECOMP_HALT_UNHANDLED 2
/* Save-data API callable from recompiled code and runtime */
int  recomp_save_init(GuestContext* c, const char* exe_path);
int  recomp_save_write(GuestContext* c, const char* name, const void* data, uint64_t size);
int  recomp_save_read(GuestContext* c, const char* name, void* buf, uint64_t buf_size, uint64_t* out_size);
int  recomp_save_delete(GuestContext* c, const char* name);
int  recomp_save_exists(GuestContext* c, const char* name);
/* Load bundled data segments into guest memory */
int  recomp_load_segments(GuestContext* c, const char* data_dir);
#endif
)RT";
}

inline const char* RuntimeC() {
    return R"RT(#include "recomp_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <io.h>
#define MKDIR(p) _mkdir(p)
#define PATH_SEP '\\'
#else
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#define MKDIR(p) mkdir(p,0755)
#define PATH_SEP '/'
#endif

static uint8_t* memptr(GuestContext* c, uint64_t va, uint64_t sz){
  uint64_t off=va-c->mem_base_vaddr;
  if(off+sz>c->mem_size) return 0;
  return c->mem+off;
}

/* Every guest access funnels through these two, so the host bridge only has to
   be checked in one place per direction. */
/* Medicao: com SUYU_RECOMP_DOUBLE_LOADS definido cada leitura passa duas vezes
   pela ponte. A diferenca de tempo entre as duas configuracoes e o custo de uma
   passagem completa por todas as leituras, medido no emulador real em vez de
   estimado. A segunda leitura acerta na cache, por isso a diferenca e um limite
   inferior do custo da primeira. */
static int _recomp_double_loads = -1;

/* Resolve um endereco do convidado num do anfitriao sem sair deste ficheiro.
   E o mesmo percurso que Memory::Impl::GetPointerImpl faz: os dezasseis bits de
   cima sao ignorados como o AArch64 os ignora, o numero de pagina indexa uma
   entrada de 32 bytes cujo primeiro campo e um ponteiro com dois bits de
   atributo no fundo, e o endereco final e esse ponteiro mais o endereco do
   convidado inteiro, nao mais um deslocamento dentro da pagina.

   Devolver nulo e sempre seguro: quem chama recua para o emulador, que e o que
   tem de acontecer para paginas nao mapeadas, memoria de depuracao e - a que
   importa para a correcao - paginas em cache do rasterizador, cujo ponteiro
   guardado e deliberadamente zero para que uma escrita avise a GPU em vez de
   lhe passar por tras. Um acesso que atravesse a fronteira de pagina tambem
   devolve nulo em vez de ler para la da entrada que consultou. */
static uint8_t* _recomp_fastptr(GuestContext* c, uint64_t a, uint32_t sz){
  const RecompHostMem* h = c->host_mem;
  uint64_t va;
  uintptr_t raw, p;
  if(!h || !h->page_table || !h->address_space_size) return 0;
  va = a & 0xFFFFFFFFFFFFULL;
  if(va + sz > h->address_space_size || va + sz < va) return 0;
  if((va & 0xFFF) + sz > 0x1000) return 0;
  memcpy(&raw,(const uint8_t*)h->page_table + (size_t)(va >> 12) * 32,sizeof raw);
  p = raw & ~(uintptr_t)3;
  if(!p) return 0;
  return (uint8_t*)(p + (uintptr_t)va);
}

#ifdef RECOMP_MEMSTATS
/* Measurement build only (-DRECOMP_MEMSTATS): per-thread counts of guest
   accesses by width and of why an access missed the fast path, printed to
   stderr every 2^27 accesses. The default build has none of this. */
#include <stdio.h>
static _Thread_local struct {
  uint64_t load[9], store[9], total;
  uint64_t slow_cross, slow_range, slow_entry, slow_nohost;
} _ms;
static void _ms_count(int store, uint32_t sz, const GuestContext* c, uint64_t a, int fast){
  if(store) _ms.store[sz]++; else _ms.load[sz]++;
  if(!fast){
    const RecompHostMem* h = c->host_mem;
    uint64_t va = a & 0xFFFFFFFFFFFFULL;
    if(!h || !h->page_table || !h->address_space_size) _ms.slow_nohost++;
    else if(va + sz > h->address_space_size || va + sz < va) _ms.slow_range++;
    else if((va & 0xFFF) + sz > 0x1000) _ms.slow_cross++;
    else _ms.slow_entry++;
  }
  if((++_ms.total & ((1ULL << 27) - 1)) == 0){
    fprintf(stderr, "[memstats %p] total=%llu ld8=%llu ld16=%llu ld32=%llu ld64=%llu "
            "st8=%llu st16=%llu st32=%llu st64=%llu slow_entry=%llu slow_cross=%llu "
            "slow_range=%llu slow_nohost=%llu\n", (void*)&_ms,
            (unsigned long long)_ms.total,
            (unsigned long long)_ms.load[1], (unsigned long long)_ms.load[2],
            (unsigned long long)_ms.load[4], (unsigned long long)_ms.load[8],
            (unsigned long long)_ms.store[1], (unsigned long long)_ms.store[2],
            (unsigned long long)_ms.store[4], (unsigned long long)_ms.store[8],
            (unsigned long long)_ms.slow_entry, (unsigned long long)_ms.slow_cross,
            (unsigned long long)_ms.slow_range, (unsigned long long)_ms.slow_nohost);
  }
}
#define RECOMP_MS(store, sz, fast) _ms_count(store, sz, c, a, fast)
#else
#define RECOMP_MS(store, sz, fast) ((void)0)
#endif

static uint64_t memload(GuestContext* c, uint64_t a, uint32_t sz){
  if(c->host_mem){
    const RecompHostMem* h = c->host_mem;
    uint8_t* fp;
    if(_recomp_double_loads < 0)
      _recomp_double_loads = getenv("SUYU_RECOMP_DOUBLE_LOADS") ? 1 : 0;
    if(_recomp_double_loads){
      volatile uint64_t _discard = h->load(h->user,a,sz);
      (void)_discard;
    }
    fp = _recomp_fastptr(c,a,sz);
    RECOMP_MS(0, sz, fp != 0);
    if(fp){ uint64_t v=0; memcpy(&v,fp,sz); return v; }
    return h->load(h->user,a,sz);
  }
  { uint8_t* p=memptr(c,a,sz); uint64_t v=0; if(p)memcpy(&v,p,sz); return v; }
}
static void memstore(GuestContext* c, uint64_t a, uint32_t sz, uint64_t v){
  if(c->host_mem){
    const RecompHostMem* h = c->host_mem;
    uint8_t* fp = _recomp_fastptr(c,a,sz);
    RECOMP_MS(1, sz, fp != 0);
    if(fp){ memcpy(fp,&v,sz); return; }
    h->store(h->user,a,sz,v);
    return;
  }
  { uint8_t* p=memptr(c,a,sz); if(p)memcpy(p,&v,sz); }
}

uint64_t recomp_load8 (GuestContext* c,uint64_t a){return memload(c,a,1);}
uint64_t recomp_load16(GuestContext* c,uint64_t a){return memload(c,a,2);}
uint64_t recomp_load32(GuestContext* c,uint64_t a){return memload(c,a,4);}
uint64_t recomp_load64(GuestContext* c,uint64_t a){return memload(c,a,8);}
void recomp_store8 (GuestContext* c,uint64_t a,uint64_t v){memstore(c,a,1,v);}
void recomp_store16(GuestContext* c,uint64_t a,uint64_t v){memstore(c,a,2,v);}
void recomp_store32(GuestContext* c,uint64_t a,uint64_t v){memstore(c,a,4,v);}
void recomp_store64(GuestContext* c,uint64_t a,uint64_t v){memstore(c,a,8,v);}


#ifndef RECOMP_STATIC_HOST
/* Owned by the runtime in the single-module shapes (standalone exe, loadable
   shared image). When several modules are linked statically into one host this
   file is compiled once for all of them, so each module defines its own
   (renamed) copy in recomp_export.c instead. */
uint64_t g_module_base = 0;
#endif

uint64_t recomp_umulh(uint64_t a,uint64_t b){
  /* Portable 64x64->high64: split into 32-bit halves. Avoids depending on
     __int128 or MSVC intrinsics so the generated project stays plain C11. */
  uint64_t al=a&0xFFFFFFFFULL, ah=a>>32, bl=b&0xFFFFFFFFULL, bh=b>>32;
  uint64_t ll=al*bl, lh=al*bh, hl=ah*bl, hh=ah*bh;
  uint64_t mid=(ll>>32)+(lh&0xFFFFFFFFULL)+(hl&0xFFFFFFFFULL);
  return hh+(lh>>32)+(hl>>32)+(mid>>32);
}
uint64_t recomp_smulh(uint64_t a,uint64_t b){
  uint64_t hi=recomp_umulh(a,b);
  /* Convert the unsigned high half to the signed one. */
  if((int64_t)a<0) hi-=b;
  if((int64_t)b<0) hi-=a;
  return hi;
}

/* ── Extensao de criptografia: AES ──
 *
 * As tabelas sao geradas a partir do inverso multiplicativo em GF(2^8) mais a
 * transformacao afim do FIPS-197, e verificadas por teste contra uma derivacao
 * independente. Ficam como constantes para nao haver inicializacao em runtime:
 * quatro threads guest a construir a mesma tabela seria uma corrida de dados.
 */
static const uint8_t _aes_sbox[256] = {
  0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
  0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
  0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
  0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
  0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
  0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
  0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
  0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
  0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
  0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
  0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
  0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
  0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
  0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
  0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
  0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};
static const uint8_t _aes_isbox[256] = {
  0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb,
  0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb,
  0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
  0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25,
  0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92,
  0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
  0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06,
  0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02, 0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b,
  0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
  0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e,
  0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b,
  0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
  0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f,
  0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef,
  0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
  0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d,
};

static uint8_t _aes_xtime(uint8_t a){ return (uint8_t)((a << 1) ^ ((a & 0x80) ? 0x1b : 0)); }
static uint8_t _aes_gmul(uint8_t a, uint8_t b){
  uint8_t p = 0; int i;
  for (i = 0; i < 8; ++i) { if (b & 1) p ^= a; a = _aes_xtime(a); b = (uint8_t)(b >> 1); }
  return p;
}
/* O estado do AES esta em coluna-maior: o byte de indice r+4c e a linha r da
   coluna c. ShiftRows roda cada linha r por r posicoes para a esquerda. */
static void _aes_shiftrows(uint8_t* o, const uint8_t* v, int inverse){
  int j;
  for (j = 0; j < 16; ++j) {
    const int r = j & 3, c = j >> 2;
    const int src = inverse ? ((c - r) & 3) : ((c + r) & 3);
    o[j] = v[r + 4 * src];
  }
}
static void _aes_mixcolumns(uint8_t* o, const uint8_t* v, int inverse){
  static const uint8_t fwd[4] = {2, 3, 1, 1};
  static const uint8_t rev[4] = {14, 11, 13, 9};
  const uint8_t* k = inverse ? rev : fwd;
  int c, r, i;
  for (c = 0; c < 4; ++c)
    for (r = 0; r < 4; ++r) {
      uint8_t acc = 0;
      for (i = 0; i < 4; ++i) acc ^= _aes_gmul(v[((r + i) & 3) + 4 * c], k[i]);
      o[r + 4 * c] = acc;
    }
}
static void _aes_get(const GuestContext* c, unsigned n, uint8_t* out){
  memcpy(out, c->vreg[n], 16);
}
static void _aes_put(GuestContext* c, unsigned d, const uint8_t* in){
  memcpy(c->vreg[d], in, 16);
}

void recomp_aese(GuestContext* c, unsigned d, unsigned n){
  uint8_t a[16], b[16], t[16]; int i;
  _aes_get(c, d, a); _aes_get(c, n, b);
  for (i = 0; i < 16; ++i) a[i] ^= b[i];
  _aes_shiftrows(t, a, 0);
  for (i = 0; i < 16; ++i) t[i] = _aes_sbox[t[i]];
  _aes_put(c, d, t);
}
void recomp_aesd(GuestContext* c, unsigned d, unsigned n){
  uint8_t a[16], b[16], t[16]; int i;
  _aes_get(c, d, a); _aes_get(c, n, b);
  for (i = 0; i < 16; ++i) a[i] ^= b[i];
  _aes_shiftrows(t, a, 1);
  for (i = 0; i < 16; ++i) t[i] = _aes_isbox[t[i]];
  _aes_put(c, d, t);
}
void recomp_aesmc(GuestContext* c, unsigned d, unsigned n){
  uint8_t a[16], t[16];
  _aes_get(c, n, a); _aes_mixcolumns(t, a, 0); _aes_put(c, d, t);
}
void recomp_aesimc(GuestContext* c, unsigned d, unsigned n){
  uint8_t a[16], t[16];
  _aes_get(c, n, a); _aes_mixcolumns(t, a, 1); _aes_put(c, d, t);
}

/* ── Extensao de criptografia: SHA-1 ──
 *
 * Segue a definicao portatil que o dynarmic usa no seu frontend A64, para que
 * o AOT e o JIT concordem. A rotacao do vetor inteiro merece cuidado: rodar
 * 128 bits 96 posicoes a direita move o elemento 3 para a posicao 0 e empurra
 * os outros, o que em ordem de elementos e [e3,e2,e1,e0] -> [e2,e1,e0,e3].
 */
static uint32_t _sha_ror32(uint32_t x, unsigned n){
  return n ? (uint32_t)((x >> n) | (x << (32 - n))) : x;
}
static uint32_t _sha_choose(uint32_t x, uint32_t y, uint32_t z){ return ((y ^ z) & x) ^ z; }
static uint32_t _sha_majority(uint32_t x, uint32_t y, uint32_t z){ return (x & y) | ((x | y) & z); }
static uint32_t _sha_parity(uint32_t x, uint32_t y, uint32_t z){ return (y ^ z) ^ x; }

/* fn: 0 = choose (SHA1C), 1 = majority (SHA1M), 2 = parity (SHA1P). */
void recomp_sha1_hash(GuestContext* c, unsigned d, unsigned n, unsigned m, int fn){
  uint32_t x[4], w[4], nv[4], y;
  int i;
  memcpy(x, c->vreg[d], 16);
  memcpy(w, c->vreg[m], 16);
  memcpy(nv, c->vreg[n], 16);
  y = nv[0];
  for (i = 0; i < 4; ++i) {
    const uint32_t low_x = x[0], after = x[1], before_high = x[2], high = x[3];
    const uint32_t t = fn == 0 ? _sha_choose(after, before_high, high)
                     : fn == 1 ? _sha_majority(after, before_high, high)
                               : _sha_parity(after, before_high, high);
    uint32_t sh[4];
    y = (uint32_t)(y + _sha_ror32(low_x, 27) + t + w[i]);
    x[1] = _sha_ror32(after, 2);
    sh[0] = x[3]; sh[1] = x[0]; sh[2] = x[1]; sh[3] = x[2];
    sh[0] = y;
    x[0] = sh[0]; x[1] = sh[1]; x[2] = sh[2]; x[3] = sh[3];
    y = high;
  }
  memcpy(c->vreg[d], x, 16);
}

void recomp_sha1su0(GuestContext* c, unsigned d, unsigned n, unsigned m){
  uint64_t dv[2], nv[2], mv[2], r[2];
  memcpy(dv, c->vreg[d], 16);
  memcpy(nv, c->vreg[n], 16);
  memcpy(mv, c->vreg[m], 16);
  r[0] = dv[1];            /* metade alta de Vd para a metade baixa */
  r[1] = nv[0];            /* metade baixa de Vn para a metade alta */
  r[0] ^= dv[0] ^ mv[0];
  r[1] ^= dv[1] ^ mv[1];
  memcpy(c->vreg[d], r, 16);
}

void recomp_sha1su1(GuestContext* c, unsigned d, unsigned n){
  uint32_t dv[4], nv[4], sh[4], t[4];
  int i;
  memcpy(dv, c->vreg[d], 16);
  memcpy(nv, c->vreg[n], 16);
  /* rodar 32 bits a direita e zerar o elemento de topo */
  sh[0] = nv[1]; sh[1] = nv[2]; sh[2] = nv[3]; sh[3] = 0;
  for (i = 0; i < 4; ++i) t[i] = dv[i] ^ sh[i];
  for (i = 0; i < 4; ++i) t[i] = _sha_ror32(t[i], 31);   /* rodar 1 a esquerda */
  t[3] = _sha_ror32(t[0], 31) ^ t[3];
  memcpy(c->vreg[d], t, 16);
}

void recomp_sha1h(GuestContext* c, unsigned d, unsigned n){
  uint32_t nv[4], r[4];
  memcpy(nv, c->vreg[n], 16);
  r[0] = _sha_ror32(nv[0], 2);
  r[1] = 0; r[2] = 0; r[3] = 0;
  memcpy(c->vreg[d], r, 16);
}

/* ── Extensao de criptografia: SHA-256 ──
 *
 * O dynarmic delega estas quatro no sha256rnds2 do anfitriao, por isso nao ha
 * referencia portatil no repositorio. A semantica aqui foi derivada da funcao
 * de ronda do FIPS-180-4 e verificada por um teste que monta um SHA-256
 * completo com as instrucoes e compara com o OpenSSL.
 *
 * Maiusculas e minusculas nao sao a mesma coisa: SIGMA0/1 entram na ronda,
 * sigma0/1 no agendamento de mensagem.
 */
static uint32_t _sha256_sig0(uint32_t x){ return _sha_ror32(x,7) ^ _sha_ror32(x,18) ^ (x >> 3); }
static uint32_t _sha256_sig1(uint32_t x){ return _sha_ror32(x,17) ^ _sha_ror32(x,19) ^ (x >> 10); }
static uint32_t _sha256_SIG0(uint32_t x){ return _sha_ror32(x,2) ^ _sha_ror32(x,13) ^ _sha_ror32(x,22); }
static uint32_t _sha256_SIG1(uint32_t x){ return _sha_ror32(x,6) ^ _sha_ror32(x,11) ^ _sha_ror32(x,25); }

/* Quatro rondas. X carrega {a,b,c,d} e Y {e,f,g,h}; W ja traz a constante K
   somada. No fim de cada ronda a concatenacao Y:X roda 32 bits a esquerda, o
   que reproduz o deslocamento das variaveis de trabalho do FIPS. */
static void _sha256_hash(uint32_t* X, uint32_t* Y, const uint32_t* W){
  int e;
  for (e = 0; e < 4; ++e) {
    const uint32_t chs = _sha_choose(Y[0], Y[1], Y[2]);
    const uint32_t maj = _sha_majority(X[0], X[1], X[2]);
    const uint32_t t = (uint32_t)(Y[3] + _sha256_SIG1(Y[0]) + chs + W[e]);
    uint32_t nx[4], ny[4];
    X[3] = (uint32_t)(t + X[3]);
    Y[3] = (uint32_t)(t + _sha256_SIG0(X[0]) + maj);
    nx[0] = Y[3]; nx[1] = X[0]; nx[2] = X[1]; nx[3] = X[2];
    ny[0] = X[3]; ny[1] = Y[0]; ny[2] = Y[1]; ny[3] = Y[2];
    memcpy(X, nx, 16); memcpy(Y, ny, 16);
  }
}

void recomp_sha256h(GuestContext* c, unsigned d, unsigned n, unsigned m){
  uint32_t X[4], Y[4], W[4];
  memcpy(X, c->vreg[d], 16); memcpy(Y, c->vreg[n], 16); memcpy(W, c->vreg[m], 16);
  _sha256_hash(X, Y, W);
  memcpy(c->vreg[d], X, 16);
}
void recomp_sha256h2(GuestContext* c, unsigned d, unsigned n, unsigned m){
  uint32_t X[4], Y[4], W[4];
  memcpy(X, c->vreg[n], 16); memcpy(Y, c->vreg[d], 16); memcpy(W, c->vreg[m], 16);
  _sha256_hash(X, Y, W);
  memcpy(c->vreg[d], Y, 16);
}
void recomp_sha256su0(GuestContext* c, unsigned d, unsigned n){
  uint32_t X[4], Y[4], T[4], r[4];
  int e;
  memcpy(X, c->vreg[d], 16); memcpy(Y, c->vreg[n], 16);
  T[0] = X[1]; T[1] = X[2]; T[2] = X[3]; T[3] = Y[0];
  for (e = 0; e < 4; ++e) r[e] = (uint32_t)(X[e] + _sha256_sig0(T[e]));
  memcpy(c->vreg[d], r, 16);
}
void recomp_sha256su1(GuestContext* c, unsigned d, unsigned n, unsigned m){
  uint32_t X[4], Y[4], Z[4], T0[4], r[4], t1[2];
  int e;
  memcpy(X, c->vreg[d], 16); memcpy(Y, c->vreg[n], 16); memcpy(Z, c->vreg[m], 16);
  T0[0] = Y[1]; T0[1] = Y[2]; T0[2] = Y[3]; T0[3] = Z[0];
  t1[0] = Z[2]; t1[1] = Z[3];
  for (e = 0; e < 2; ++e) r[e] = (uint32_t)(X[e] + T0[e] + _sha256_sig1(t1[e]));
  t1[0] = r[0]; t1[1] = r[1];
  for (e = 2; e < 4; ++e) r[e] = (uint32_t)(X[e] + T0[e] + _sha256_sig1(t1[e - 2]));
  memcpy(c->vreg[d], r, 16);
}

/* ── Reciprocal estimates ──
 *
 * FRECPE and FRSQRTE return a deliberately imprecise result that the
 * architecture pins down exactly: an eight-bit estimate read off a table,
 * placed at the top of the mantissa. Computing 1/x instead would be more
 * accurate and would still be wrong, because the guest goes on to refine the
 * estimate with FRECPS/FRSQRTS and compares against what the hardware - and
 * the JIT backend, which implements the same tables - would have produced.
 */
static unsigned recomp_recip_estimate(uint64_t a){
  /* a is a u0.9 fraction in [0.5, 1.0), i.e. an integer in [256, 512). */
  a = a * 2 + 1;
  uint64_t b = (UINT64_C(1) << 19) / a;
  return (unsigned)((b + 1) / 2);
}

static unsigned recomp_recip_sqrt_estimate(uint64_t a){
  /* a is a u0.9 fraction in [0.25, 1.0), i.e. an integer in [128, 512). */
  a &= 0x1FF;
  if (a < 256) a = a * 2 + 1;          /* [0.25, 0.5) */
  else         a = (a | 1) * 2;        /* [0.5, 1.0) */
  uint64_t b = 512;
  while (a * (b + 1) * (b + 1) < (UINT64_C(1) << 28)) b++;
  return (unsigned)((b + 1) / 2);
}

/* FPUnpack, reduced to the FPCR the generated code runs under: round to
   nearest even, no flush-to-zero, no default-NaN. Returns 0 for a normal or
   subnormal number, 1 for zero, 2 for infinity, 3 for a quiet NaN and 4 for a
   signalling one. The mantissa comes back normalised with its point at bit 62,
   which is the form the estimate tables index. */
static int recomp_fp_unpack(uint64_t bits,int is64,int* sign,int* exp,uint64_t* mant){
  const unsigned mw = is64 ? 52u : 23u, ew = is64 ? 11u : 8u;
  const int bias = is64 ? 1023 : 127;
  const uint64_t frac_mask = (UINT64_C(1) << mw) - 1;
  const uint64_t exp_max = (UINT64_C(1) << ew) - 1;
  const uint64_t raw_exp = (bits >> mw) & exp_max;
  const uint64_t frac = bits & frac_mask;
  *sign = (int)((bits >> (mw + ew)) & 1);
  if (raw_exp == 0) {
    if (frac == 0) { *exp = 0; *mant = 0; return 1; }
    { /* Subnormal: shift the fraction up until bit 62 is set. */
      const int denormal_exp = (1 - bias) - (int)mw;
      int hb = 63;
      while (hb >= 0 && !((frac >> hb) & 1)) hb--;
      { const int off = 62 - hb;
        *mant = frac << off;
        *exp = denormal_exp - off + 62; }
      return 0;
    }
  }
  if (raw_exp == exp_max) {
    if (frac == 0) { *exp = 1000000; *mant = UINT64_C(1) << 62; return 2; }
    return ((frac >> (mw - 1)) & 1) ? 3 : 4;
  }
  *exp = (int)raw_exp - bias;
  *mant = (frac | (UINT64_C(1) << mw)) << (62 - mw);
  return 0;
}

uint64_t recomp_frecpe(uint64_t bits,int is64){
  const unsigned mw = is64 ? 52u : 23u, ew = is64 ? 11u : 8u;
  const int bias = is64 ? 1023 : 127, emin = is64 ? -1022 : -126;
  const uint64_t inf = ((UINT64_C(1) << ew) - 1) << mw;
  int sign = 0, exp = 0; uint64_t mant = 0;
  const int type = recomp_fp_unpack(bits, is64, &sign, &exp, &mant);
  const uint64_t sbit = (uint64_t)sign << (mw + ew);
  if (type >= 3) return bits | (UINT64_C(1) << (mw - 1));   /* quiet the NaN */
  if (type == 2) return sbit;                                /* infinity -> zero */
  if (type == 1) return sbit | inf;                          /* zero -> infinity */
  /* Too small to reciprocate: round to nearest sends this to infinity. */
  if (exp < emin - 2) return sbit | inf;
  { uint64_t estimate = (uint64_t)recomp_recip_estimate(mant >> (62 - 8)) << (mw - 8);
    int result_exp = -(exp + 1);
    if (result_exp < emin) {
      /* The result is subnormal: put the implicit bit back and shift it down. */
      estimate |= UINT64_C(1) << mw;
      if (result_exp == emin - 1) { estimate >>= 1; }
      else                        { estimate >>= 2; result_exp++; }
    }
    return sbit | ((uint64_t)(result_exp + bias) << mw) |
           (estimate & ((UINT64_C(1) << mw) - 1));
  }
}

uint64_t recomp_frsqrte(uint64_t bits,int is64){
  const unsigned mw = is64 ? 52u : 23u, ew = is64 ? 11u : 8u;
  const int bias = is64 ? 1023 : 127;
  const uint64_t inf = ((UINT64_C(1) << ew) - 1) << mw;
  int sign = 0, exp = 0; uint64_t mant = 0;
  const int type = recomp_fp_unpack(bits, is64, &sign, &exp, &mant);
  const uint64_t sbit = (uint64_t)sign << (mw + ew);
  if (type >= 3) return bits | (UINT64_C(1) << (mw - 1));   /* quiet the NaN */
  if (type == 1) return sbit | inf;                          /* zero -> infinity */
  if (sign) return inf | (UINT64_C(1) << (mw - 1));          /* negative -> default NaN */
  if (type == 2) return 0;                                   /* +infinity -> +zero */
  { /* Halve the exponent, rounding down; a right shift of a negative value is
       only implementation-defined, so the floor is written out. */
    const int neg = -(exp + 1);
    const int result_exp = neg >= 0 ? (neg >> 1) : -(((-neg) + 1) >> 1);
    /* An odd exponent needs one more mantissa bit to index the table. */
    const int odd = (exp % 2) == 0;
    const uint64_t est = recomp_recip_sqrt_estimate(mant >> (62 - (odd ? 7 : 8)));
    return ((uint64_t)(result_exp + bias) << mw) |
           ((est << (mw - 8)) & ((UINT64_C(1) << mw) - 1));
  }
}

uint64_t recomp_add_with_carry(GuestContext* c,uint64_t a,uint64_t b,int carry,int is64,int set_flags){
  /* ARM's AddWithCarry(a,b,carry). ADC is AddWithCarry(Rn,Rm,C) and SBC is
     AddWithCarry(Rn,~Rm,C), so one primitive covers both: the caller inverts.
     The inversion is done before the width mask below, which is why a 32-bit
     ~Rm comes out as the 32-bit NOT and not as a sign-extended 64-bit one. */
  uint64_t m=is64?~0ULL:0xFFFFFFFFULL;
  uint64_t s=is64?0x8000000000000000ULL:0x80000000ULL;
  a&=m; b&=m;
  uint64_t t=a+b;
  uint64_t r=(t+(uint64_t)carry)&m;
  if(set_flags){
    /* At 64 bits a+b can already wrap before the carry is added, so the
       carry-out has to be taken from both steps. At 32 bits the masked
       operands leave room, so bit 32 of the full sum is the carry. */
    int cout = is64 ? (((t<a)?1:0) | (((t+(uint64_t)carry)<t)?1:0))
                    : (int)(((a+b+(uint64_t)carry)>>32)&1);
    c->z=(r==0); c->n=(r&s)?1:0;
    c->c=cout?1:0;
    /* Carry-in shifts the result by one but cannot change whether the two
       operands had the same sign, so the ordinary overflow test still holds. */
    c->v=((~(a^b)&(a^r))&s)?1:0;
  }
  return r;
}


/* ── Save-data filesystem ── */

/* snprintf(dst,sizeof dst,"%s",src) is exactly what every straight bounded
 * copy below used to do, but GCC's -Wformat-truncation can't prove src is
 * short enough for dst and (with -Werror) fails the build over it, even
 * though snprintf itself never overflows - it just truncates safely, which
 * is already the desired behaviour for a path that's too long. Do the same
 * truncating copy without going through a "%s" format string, which is the
 * part the warning actually keys on. */
static void safe_strcpy(char* dst, size_t dst_size, const char* src) {
  if (dst_size == 0) return;
  size_t n = strlen(src);
  if (n >= dst_size) n = dst_size - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

static void mkpath(const char* path) {
  char tmp[512]; size_t len;
  safe_strcpy(tmp,sizeof tmp,path); len=strlen(tmp);
  for(size_t i=1;i<len;i++){
    if(tmp[i]==PATH_SEP||tmp[i]=='/'){tmp[i]=0; MKDIR(tmp); tmp[i]=PATH_SEP;}
  }
  MKDIR(tmp);
}

int recomp_save_init(GuestContext* c, const char* exe_path) {
  char dir[512];
  /* Put save_data/ next to the executable */
  safe_strcpy(dir,sizeof dir,exe_path);
  char* sl=strrchr(dir,PATH_SEP);
  if(!sl) sl=strrchr(dir,'/');
  if(sl) *(sl+1)=0; else dir[0]=0;
  snprintf(c->save_dir,sizeof c->save_dir,"%ssave_data",dir);
  mkpath(c->save_dir);
  printf("[recomp] Save directory: %s\n",c->save_dir);
  return 1;
}

int recomp_save_write(GuestContext* c, const char* name, const void* data, uint64_t size) {
  char path[1024];
  snprintf(path,sizeof path,"%s%c%s",c->save_dir,PATH_SEP,name);
  /* Ensure parent dirs exist */
  char parent[1024]; safe_strcpy(parent,sizeof parent,path);
  char* sl=strrchr(parent,PATH_SEP); if(!sl) sl=strrchr(parent,'/'); if(sl)*sl=0;
  mkpath(parent);
  FILE* f=fopen(path,"wb");
  if(!f){fprintf(stderr,"[recomp] save write failed: %s\n",path); return 0;}
  fwrite(data,1,(size_t)size,f); fclose(f);
  printf("[recomp] Saved %llu bytes -> %s\n",(unsigned long long)size,path);
  return 1;
}

int recomp_save_read(GuestContext* c, const char* name, void* buf, uint64_t buf_size, uint64_t* out_size) {
  char path[1024];
  snprintf(path,sizeof path,"%s%c%s",c->save_dir,PATH_SEP,name);
  FILE* f=fopen(path,"rb");
  if(!f){if(out_size)*out_size=0; return 0;}
  fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
  uint64_t to_read=(uint64_t)sz<buf_size?(uint64_t)sz:buf_size;
  fread(buf,1,(size_t)to_read,f); fclose(f);
  if(out_size)*out_size=to_read;
  printf("[recomp] Loaded %llu bytes <- %s\n",(unsigned long long)to_read,path);
  return 1;
}

int recomp_save_delete(GuestContext* c, const char* name) {
  char path[1024];
  snprintf(path,sizeof path,"%s%c%s",c->save_dir,PATH_SEP,name);
  return remove(path)==0;
}

int recomp_save_exists(GuestContext* c, const char* name) {
  char path[1024];
  snprintf(path,sizeof path,"%s%c%s",c->save_dir,PATH_SEP,name);
  FILE* f=fopen(path,"rb");
  if(f){fclose(f); return 1;} return 0;
}

/* ── Segment loader: loads rodata.bin + data.bin from the data dir into guest memory ── */

int recomp_load_segments(GuestContext* c, const char* data_dir) {
  const char* names[]={"rodata.bin","data.bin","text.bin"};
  /* Corresponding offsets from segment info embedded in manifest — for now, load
     sequentially after .text in memory. The real offsets come from the blockmap. */
  for(int i=0;i<3;i++){
    char path[1024];
    snprintf(path,sizeof path,"%s%c%s",data_dir,PATH_SEP,names[i]);
    FILE* f=fopen(path,"rb");
    if(!f) continue;
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    if((uint64_t)sz<=c->mem_size){
      /* Load at the appropriate offset — text at base, others after */
      fread(c->mem,1,(size_t)sz,f);
    }
    fclose(f);
    printf("[recomp] Loaded segment %s (%ld bytes)\n",names[i],sz);
  }
  return 1;
}

/* ── SVC handler with HLE filesystem support ── */

#ifdef SUYU_HOSTED_RECOMP
/* In the suyu-hosted build the block already set pending_svc before calling
   this. Just return — arm_recomp.cpp's run loop detects pending_svc != ~0 and
   dispatches to the real HLE kernel. */
void recomp_svc(GuestContext* c,unsigned imm){ (void)c; (void)imm; }
#else
void recomp_svc(GuestContext* c,unsigned imm){
  /* Standalone runtime: this handler services the call itself, so clear the
     pending flag. The suyu-hosted build replaces this translation unit with a
     bridge that leaves pending_svc set and returns, handing the call to the
     real HLE kernel instead. */
  c->pending_svc = ~0ULL;
  switch(imm){
  case 0x1: /* SetHeapSize — x1 = requested size */
    if(c->heap_base==0){
      c->heap_base=c->mem_base_vaddr+c->mem_size/2;
      c->heap_cur=c->heap_base;
      c->heap_end=c->heap_base+c->mem_size/2;
    }
    c->x[0]=0; /* success */
    c->x[1]=c->heap_base;
    break;
  case 0x2: /* SetMemoryPermission — stub success */
    c->x[0]=0;
    break;
  case 0x3: /* SetMemoryAttribute — stub success */
    c->x[0]=0;
    break;
  case 0x6: /* QueryMemory — stub: report all memory as readable/writable */
    c->x[0]=0;
    c->x[1]=0; /* MemoryInfo written to [x0] — simplified */
    break;
  case 0x7: /* ExitProcess */
    printf("[recomp] ExitProcess called\n");
    c->halted=1;
    break;
  case 0x8: /* CreateThread — stub, return handle=1 */
    c->x[0]=0; c->x[1]=1;
    break;
  case 0xB: /* SleepThread — yield CPU to prevent spin-lock lag */
#ifdef _WIN32
    Sleep((DWORD)(c->x[0] / 1000000ULL)); /* ns to ms */
#else
    { struct timespec ts; ts.tv_sec=0; ts.tv_nsec=(long)(c->x[0]>0?c->x[0]:1000000);
      nanosleep(&ts,0); }
#endif
    c->x[0]=0;
    break;
  case 0x15: /* SendSyncRequest — IPC for fsp-srv / save data */
    /* Simplified HLE: check x[0] for handle, interpret IPC command buffer.
       For now, stub success so game save code paths don't crash. */
    c->x[0]=0;
    break;
  case 0x16: /* SendSyncRequestWithUserBuffer */
    c->x[0]=0;
    break;
  case 0x18: /* CloseHandle — stub */
    c->x[0]=0;
    break;
  case 0x1A: /* WaitSynchronization — stub immediate return */
    c->x[0]=0; c->x[1]=0;
    break;
  case 0x1F: /* ConnectToNamedPort — stub, return handle */
    c->x[0]=0; c->x[1]=0x100;
    break;
  case 0x21: /* SendSyncRequest (sm: variant) */
    c->x[0]=0;
    break;
  case 0x26: /* Break — debug break */
    printf("[recomp] Break SVC x0=%llu\n",(unsigned long long)c->x[0]);
    break;
  case 0x27: /* OutputDebugString */
    { uint8_t* p=memptr(c,c->x[0],(uint64_t)c->x[1]);
      if(p) printf("[guest] %.*s\n",(int)c->x[1],(char*)p);
      c->x[0]=0;
    }
    break;
  case 0x29: /* GetInfo — return stub values for system info queries */
    { uint32_t id=(uint32_t)c->x[1];
      switch(id){
      case 0: c->x[1]=0xFFFFFF; break; /* AllowedCPUCoreMask */
      case 1: c->x[1]=0xF; break; /* AllowedThreadPrioMask */
      case 2: c->x[1]=c->mem_base_vaddr; break; /* MapRegionBaseAddr */
      case 3: c->x[1]=c->mem_size; break; /* MapRegionSize */
      case 4: c->x[1]=c->heap_base; break; /* HeapRegionBaseAddr */
      case 5: c->x[1]=c->heap_end-c->heap_base; break; /* HeapRegionSize */
      case 6: c->x[1]=c->mem_size; break; /* TotalMemorySize */
      case 7: c->x[1]=c->mem_size/2; break; /* UsedMemorySize */
      case 12: c->x[1]=c->mem_base_vaddr+c->mem_size; break; /* AslrRegionBaseAddr */
      case 13: c->x[1]=0x1000000; break; /* AslrRegionSize */
      case 14: c->x[1]=c->mem_base_vaddr+c->mem_size; break; /* StackRegionBaseAddr */
      case 15: c->x[1]=0x100000; break; /* StackRegionSize */
      default: c->x[1]=0; break;
      }
      c->x[0]=0;
    }
    break;
  default:
    printf("[recomp] Unhandled SVC #0x%x x0=0x%llx x1=0x%llx\n",imm,
      (unsigned long long)c->x[0],(unsigned long long)c->x[1]);
    c->x[0]=0;
    break;
  }
}
#endif /* SUYU_HOSTED_RECOMP */

/* An opcode the decoder does not implement. Stubbing it out (the old
   behaviour: zero x0 and carry on) silently corrupts guest state - a
   function whose body is one unimplemented SIMD instruction returns a
   plausible-looking 0, and the caller then dereferences it. That is
   indistinguishable from a real null and shows up much later as a crash
   nowhere near the actual gap.

   Instead, park the context at this exact PC and halt with a distinct code.
   The host (ArmRecomp::RunThread) treats halted == RECOMP_HALT_UNHANDLED as
   "run this address on the interpreter fallback instead", so the instruction
   executes correctly and control returns to recompiled code as soon as the
   PC is covered again. Correct by construction whatever the decoder does or
   does not cover, and every remaining gap costs speed rather than
   correctness.

   Standalone builds have no fallback engine to hand off to, so they keep the
   old step-over behaviour - degraded, but still the best available there. */
/* DMB/DSB/ISB. Under the hosted backend the guest is genuinely multi-threaded,
   so these must be real fences: without one the host compiler is free to sink a
   pointer store past the field stores the barrier was there to publish. The
   standalone runtime drives one guest thread on one host thread and has nothing
   to order against, so it keeps the free no-op. */
void recomp_barrier(void){
#ifdef SUYU_HOSTED_RECOMP
#if defined(__cplusplus)
    atomic_thread_fence(memory_order_seq_cst);
#elif defined(_MSC_VER)
    MemoryBarrier();
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
#endif
}

void recomp_exclusive(GuestContext* c,uint32_t insn,uint64_t pc){
  if(c->host_mem && c->host_mem->exclusive && c->host_mem->exclusive(c->host_mem->user,insn)) return;
  c->pc=pc;
  c->halted=RECOMP_HALT_UNHANDLED;
}

void recomp_unhandled(GuestContext* c,uint32_t insn,uint64_t pc){
#ifdef SUYU_HOSTED_RECOMP
  /* A small set of host-dependent instructions (clock registers and exclusive
     memory operations) uses native runtime helpers, never a JIT. */
  if(c->host_mem && c->host_mem->exclusive && c->host_mem->exclusive(c->host_mem->user,insn)) return;
  c->pc=pc;
  c->halted=RECOMP_HALT_UNHANDLED;
#else
  fprintf(stderr,"[recomp] unhandled insn 0x%08x at 0x%llx\n",insn,(unsigned long long)pc);
  (void)c; /* stepping over is bad enough; do not clobber a register too */
#endif
}

#ifndef RECOMP_STATIC_HOST
/* Drives a single module's own dispatch table. Meaningless when the runtime is
   shared between several statically linked modules - the host dispatches
   across them instead - so it is compiled out there, where recomp_lookup has
   been renamed per module and would not resolve. */
void recomp_run(GuestContext* c){
  uint64_t g=0;
  while(!c->halted){
    BlockFn f=recomp_lookup(c->pc);
    if(!f){ fprintf(stderr,"[recomp] no block at 0x%llx\n",(unsigned long long)c->pc); break;}
    f(c);
    if(++g>100000000ULL){ fprintf(stderr,"[recomp] watchdog (100M iterations)\n"); break; }
    /* Yield every 4096 blocks to prevent 100% CPU spin on tight loops */
    if((g & 0xFFF)==0){
#ifdef _WIN32
      Sleep(0);
#else
      { struct timespec ts={0,0}; nanosleep(&ts,0); }
#endif
    }
  }
}
#endif /* !RECOMP_STATIC_HOST */
)RT";
}

} // namespace suyu::recomp
