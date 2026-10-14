#!/usr/bin/env python3
"""Regenerate an existing Source export and build a Linux per-game executable.

The original export and user data are never modified. Paths with spaces/brackets
are supported. A Source export is the prerequisite; this tool does not decrypt NSPs.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import struct
import subprocess

REPO = Path(__file__).resolve().parents[2]
ORDER = ['rtld', 'main', *(f'subsdk{i}' for i in range(10)), 'sdk']

def run(args):
    print(shlex.join(map(str, args)), flush=True)
    subprocess.run(list(map(str, args)), check=True)

def registration(modules):
    out = ['#include <stdint.h>', 'typedef void (*BlockFn)(void*);',
           'typedef struct { const char* name; BlockFn (*lookup)(uint64_t); void (*set_base)(uint64_t); } Module;']
    for name in modules:
        out += [f'extern BlockFn recomp_image_lookup_{name}(uint64_t);',
                f'extern void recomp_image_set_base_{name}(uint64_t);']
    out += ['static const Module modules[] = {']
    out += [f'{{"{name}", recomp_image_lookup_{name}, recomp_image_set_base_{name}}},' for name in modules]
    out += ['};', 'const Module* suyu_recomp_static_modules(unsigned* count);',
            'const Module* suyu_recomp_static_modules(unsigned* count) {',
            '*count = sizeof(modules) / sizeof(modules[0]); return modules; }']
    return '\n'.join(out) + '\n'

def jump_table_roots(segments):
    """Case labels reached through a switch jump table.

    A C++ switch compiles to a table of signed 32-bit displacements relative to
    the table's own address, reached by this idiom:

        adrp  Xb, page          ; b0002909  adrp x9, #0xbec000
        add   Xb, Xb, #imm      ; 91133129  add  x9, x9, #0x4cc
        ldrsw Xt, [Xb, Xi, lsl #2]  ; b8a87928
        add   Xt, Xt, Xb        ; 8b090108
        br    Xt                ; d61f0100

    None of the other sources can see those labels. They are not branch
    targets, nothing relocates against them, and the table holds four-byte
    relative displacements rather than the eight-byte absolute pointers the
    data scan looks for. Without this, every case label of every switch in the
    game is missing, the block that ends in the BR hands the dispatcher an
    address with no block, and the thread drops to the JIT - which is what was
    observed in play: main+0x6CB6D8 and main+0x6CB700, entries 1 and 2 of the
    table at main+0xBEC4CC, dozens of times a second.

    The table's length is not encoded in the table. It lives in the bounds
    check the compiler emits just before, `cmp Wi, #N` against the same index
    register, and N + 1 is the entry count. Taking that seriously matters:
    reading instead until an entry stops looking like a .text address walks off
    the end of every table into whatever follows and produced 461 838 roots for
    main instead of a few thousand. A table whose bound cannot be found is left
    alone rather than guessed at.
    """
    text_lo, text = segments[0]
    text_hi = text_lo + len(text)

    def read32(address):
        for base, data in segments:
            offset = address - base
            if 0 <= offset and offset + 4 <= len(data):
                return struct.unpack_from('<i', data, offset)[0]
        return None

    words = struct.unpack(f'<{len(text) // 4}I', text[:len(text) // 4 * 4])
    found = set()
    for i, word in enumerate(words):
        if (word & 0xFFFFFC1F) != 0xD61F0000:            # BR Xt
            continue
        target_reg = (word >> 5) & 31
        if i < 2:
            continue
        # The compiler does not always leave these three adjacent - it schedules
        # unrelated work between them - so look in a small window rather than at
        # fixed positions. Insisting on exact positions missed 166 tables.
        add_index = None
        for k in range(i - 1, max(i - 5, -1), -1):
            if (words[k] & 0xFFE0FC00) == 0x8B000000 and (words[k] & 31) == target_reg:
                add_index = k
                break
        if add_index is None:
            continue
        add_back = words[add_index]
        load_index = None
        for k in range(add_index - 1, max(add_index - 5, -1), -1):
            if (words[k] & 0xFFE0FC00) == 0xB8A07800:    # LDRSW Xt, [Xb, Xi, lsl #2]
                load_index = k
                break
        if load_index is None:
            continue
        load = words[load_index]
        loaded_reg = load & 31
        base_reg = (load >> 5) & 31
        index_reg = (load >> 16) & 31
        # The add folds the loaded displacement back onto the table address, and
        # the operands come in either order - `add x8, x8, x9` in one function
        # and `add x9, x10, x9` in the next. Requiring one spelling silently
        # missed every switch that used the other.
        operands = {(add_back >> 5) & 31, (add_back >> 16) & 31}
        if operands != {loaded_reg, base_reg}:
            continue
        # Walk back for the ADRP that forms the table address, the ADD that
        # completes it, and the compare that bounds it.
        table, addend, count = None, 0, None
        for k in range(load_index - 1, max(load_index - 24, -1), -1):
            candidate = words[k]
            if (candidate & 0xFF800000) == 0x71000000 and (candidate & 31) == 31 \
                    and ((candidate >> 5) & 31) == index_reg and count is None:
                immediate = (candidate >> 10) & 0xFFF
                if (candidate >> 22) & 1:
                    immediate <<= 12
                # Whether the bound is the immediate or one past it depends on
                # the branch the compare feeds: `b.hi` sends index > N to the
                # default, so N is a valid index and there are N + 1 entries;
                # `b.hs` sends index >= N, so there are N. Both spellings occur.
                follower = words[k + 1] if k + 1 < len(words) else 0
                if (follower & 0xFF000010) == 0x54000000:
                    condition = follower & 15
                    if condition == 0x8:        # HI
                        count = immediate + 1
                    elif condition == 0x2:      # CS/HS
                        count = immediate
                continue
            if (candidate & 0xFFC00000) == 0x91000000 and (candidate & 31) == base_reg \
                    and ((candidate >> 5) & 31) == base_reg:
                addend = (candidate >> 10) & 0xFFF
                if (candidate >> 22) & 1:
                    addend <<= 12
                continue
            if (candidate & 0x9F000000) == 0x90000000 and (candidate & 31) == base_reg \
                    and table is None:
                immlo = (candidate >> 29) & 3
                immhi = (candidate >> 5) & 0x7FFFF
                page = (immhi << 2) | immlo
                if page & (1 << 20):
                    page -= 1 << 21
                table = ((text_lo + k * 4) & ~0xFFF) + (page << 12) + addend
                # The compare that bounds the table sits *before* the ADRP, so
                # stopping here would never reach it. Keep walking back.
                continue
            if table is not None and count is not None:
                break
        if table is None or count is None or not 0 < count <= 4096:
            continue
        for entry in range(count):
            displacement = read32(table + entry * 4)
            if displacement is None:
                break
            destination = table + displacement
            if not (text_lo <= destination < text_hi) or destination % 4:
                break
            found.add(destination)
    return found


def eh_frame_roots(rodata_segment, text_lo, text_hi):
    """Function starts taken from the unwind tables.

    A C++ binary carries an .eh_frame_hdr whose binary-search table lists every
    function that can be unwound through, which is nearly all of them. That is
    an authoritative list of entry points, and it reaches functions no other
    source does: a function only ever called through a vtable or a jump table
    leaves no direct branch, no relocation and no data-word pointer, so it stays
    invisible until something jumps to it at run time and the block is missing.

    The dumped segments have no section headers, so the header is found by its
    own signature and then validated rather than trusted: the table has to be
    strictly increasing and land inside .text, which a false positive will not
    do. The table's entries are signed 32-bit displacements; which base they are
    relative to is spelled by the encoding byte, but rather than trust that on
    an unlabelled blob both plausible bases are tried and the one that validates
    is used.
    """
    base_va, data = rodata_segment
    found = set()
    start = 0
    while True:
        index = data.find(b'\x01\x1b\x03\x3b', start)
        if index < 0:
            return found
        start = index + 4
        try:
            count = struct.unpack_from('<I', data, index + 8)[0]
        except struct.error:
            continue
        table = index + 12
        if not 0 < count < 1_000_000 or table + count * 8 > len(data):
            continue
        for relative_to_entry in (True, False):
            addresses = []
            previous = -1
            ok = True
            inside = 0
            for i in range(count):
                entry = table + i * 8
                displacement = struct.unpack_from('<i', data, entry)[0]
                anchor_va = base_va + (entry if relative_to_entry else index)
                value = anchor_va + displacement
                if value <= previous:          # the table is sorted by design
                    ok = False
                    break
                previous = value
                if text_lo <= value < text_hi and value % 4 == 0:
                    inside += 1
                    addresses.append(value)
            # A real table is almost entirely inside .text; a coincidence is not.
            if ok and inside * 10 >= count * 9:
                found.update(addresses)
                break
    return found


def discover_roots(directory, module, use_unwind=False):
    """Seed indirect targets from data, dynsym and RELA before block discovery."""
    segments = [(module[name + '_vaddr'], (directory / 'data' / (name + '.bin')).read_bytes())
                for name in ('text', 'rodata', 'data')]
    lo, text = segments[0]
    hi = lo + len(text)
    roots = set()
    def add(value):
        if lo <= value < hi and value % 4 == 0:
            roots.add(value)
    def read(address, size=8):
        for base, data in segments:
            offset = address - base
            if 0 <= offset and offset + size <= len(data):
                return int.from_bytes(data[offset:offset + size], 'little')
        return 0
    for _, data in segments[1:]:
        for offset in range(0, len(data) - 7, 8):
            add(struct.unpack_from('<Q', data, offset)[0])
    if use_unwind:
        for address in eh_frame_roots(segments[1], lo, hi):
            add(address)
    for address in jump_table_roots(segments):
        add(address)
    mod0 = lo + read(lo + 4, 4)
    dynamic = {}
    if read(mod0, 4) == 0x30444f4d:
        start = mod0 + read(mod0 + 4, 4)
        for address in range(start, start + 0x1000, 16):
            tag = read(address)
            if not tag:
                break
            dynamic[tag] = read(address + 8)
    symtab, strtab = dynamic.get(6, 0), dynamic.get(5, 0)
    if symtab and strtab > symtab:
        for i in range(1, min((strtab - symtab) // 24, 65536)):
            address = symtab + i * 24
            if read(address + 6, 2):
                add(read(address + 8))
    for address, length in ((dynamic.get(7, 0), dynamic.get(8, 0)),
                            (dynamic.get(23, 0), dynamic.get(2, 0))):
        if length > sum(len(data) for _, data in segments):
            raise ValueError('Invalid relocation table size')
        for p in range(address, address + length - 23, 24):
            info, addend = read(p + 8), read(p + 16)
            kind, symbol = info & 0xffffffff, info >> 32
            if kind == 0x403:
                add(addend)
            elif kind in (0x101, 0x401, 0x402) and symtab and symbol:
                add(read(symtab + symbol * 24 + 8) + (addend if kind == 0x101 else 0))
    entry = 0
    off = (read(lo + 4, 4) + 0x1c + 3) & ~3
    while off + 4 <= len(text):
        if read(lo + off, 4):
            entry = lo + off
            break
        off += 4
    add(entry)
    return roots, entry

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--export', required=True, type=Path, help='Source export exefs directory')
    parser.add_argument('--output', required=True, type=Path, help='New directory for regenerated export')
    parser.add_argument('--build-dir', required=True, type=Path)
    parser.add_argument('--rom', required=True, type=Path)
    parser.add_argument('--user-dir', required=True, type=Path)
    parser.add_argument('--jobs', type=int, default=8)
    parser.add_argument('--cmake', default='cmake')
    parser.add_argument('--generate-only', action='store_true')
    parser.add_argument('--coverage-report', type=Path, action='append', default=[],
                        help='Add verified module-relative entry points observed in a run')
    parser.add_argument('--unwind-roots', action='store_true',
                        help='Seed roots from the .eh_frame unwind tables as well. Off by '
                             'default: measured at +1.3%% run time and +5 MB for 40 693 extra '
                             'blocks, and it closed none of the observed gaps, which are all '
                             'indirect targets or points where the JIT handed control back '
                             'mid-block. Worth turning on only with a coverage report showing '
                             'misses it would have caught.')
    parser.add_argument('--title', default='ASTRAL CHAIN')
    args = parser.parse_args()
    source, output, build = args.export.resolve(), args.output.resolve(), args.build_dir.resolve()
    if output == source or source in output.parents:
        parser.error('Output must be separate from the original export')
    if output.exists() and any(output.iterdir()):
        parser.error('Output must be empty; use a new directory for each export')
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    manifest_path = source / 'aot_manifest.json'
    if not manifest_path.exists():
        manifest_path = source.parent / 'aot_manifest.json'
    if manifest_path.exists():
        manifest = json.loads(manifest_path.read_text())
    else:
        manifest = {'modules': [json.loads(p.read_text()) for p in sorted(source.glob('*/module.json'))]}
    if not manifest['modules']:
        parser.error('No module manifest or captured inputs found')
    modules = {m['name']: m for m in manifest['modules']}
    unknown = set(modules) - set(ORDER)
    if unknown:
        parser.error(f'Unsupported NSO module names: {unknown}')
    ordered = [name for name in ORDER if name in modules]
    coverage = {}
    for report_path in args.coverage_report:
        report = json.loads(report_path.read_text())
        identities = {m['name']: m['sha256'] for m in report['modules']}
        for site in report['fallback_sites']:
            name = site['module']
            if name not in modules:
                continue
            actual = [hashlib.sha256((source / name / 'data' / f'{segment}.bin').read_bytes()).hexdigest()
                      for segment in ('text', 'rodata', 'data')]
            if identities.get(name) != actual:
                raise ValueError(f'Coverage report does not match module {name}')
            coverage.setdefault(name, set()).add(site['offset'])
    output.mkdir(parents=True, exist_ok=True)
    generator = output / 'export_module'
    run([os.environ.get('CXX', 'c++'), '-std=c++20', '-O2', '-I', REPO / 'src',
         REPO / 'tools/aot/export_module.cpp', '-lcrypto', '-o', generator])
    for name in ordered:
        old = source / name
        # Preserve every entry point previously discovered by the UI's symbol analysis.
        roots, entry_pc = discover_roots(old, modules[name], args.unwind_roots)
        roots.update(coverage.get(name, set()))
        for unit in sorted((old / 'src').glob('*.c')):
            roots.update(int(value, 16) for value in
                         re.findall(r'\{0x([0-9a-fA-F]+)ULL,\s*blk_', unit.read_text()))
        roots_file = output / f'{name}.roots'
        roots_file.write_text('\n'.join(f'{value:x}' for value in sorted(roots)) + '\n')
        old_export = old / 'recomp_export.c'
        if old_export.exists():
            entry = re.search(r'recomp_image_entry\(void\).*?return (0x[0-9a-fA-F]+)',
                              old_export.read_text())
            if entry:
                entry_pc = int(entry.group(1), 16)
        run([generator, name, old / 'data', output / name, modules[name]['text_vaddr'],
             hex(entry_pc), roots_file, args.title])
        identity = json.loads((output / name / 'recomp_identity.json').read_text())
        modules[name]['segment_sha256'] = identity['sha256']
    (output / 'recomp_registration.c').write_text(registration(ordered))
    manifest.update(version=3, runtime_abi_version=identity['abi_version'], source_export=str(source),
                    modules=[modules[name] for name in ordered])
    (output / 'aot_manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    launcher = output / 'play.sh'
    command = [build / 'bin/suyu-cmd-static', '--user-dir', args.user_dir.resolve(),
               '-g', args.rom.resolve()]
    launcher.write_text('#!/bin/sh\nexec ' + shlex.join(map(str, command)) + ' "$@"\n')
    launcher.chmod(0o755)
    (output / 'build.sh').write_text('#!/bin/sh\nset -eu\n' +
        shlex.join([args.cmake, '-S', str(REPO), '-B', str(build), '-DENABLE_QT=OFF',
                    '-DVulkanHeaders_FORCE_BUNDLED=ON', '-DSUYU_CMD_RECOMP_DIR=' + str(output)]) + '\n' +
        shlex.join([args.cmake, '--build', str(build), '--target', 'suyu-cmd-static', '-j', str(args.jobs)]) + '\n')
    if not args.generate_only:
        run(['sh', output / 'build.sh'])

if __name__ == '__main__':
    main()
