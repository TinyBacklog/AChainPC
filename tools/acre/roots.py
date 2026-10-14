"""Indirect-branch roots for block discovery, ported unchanged from suyu's build_game.py.

The static pass only finds code it can reach through direct branches. Everything
reached indirectly - callbacks, vtables, switch cases, exported symbols - has to
be seeded as a root first, from the module's data, dynamic symbols and
relocations.
"""
import struct


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


def discover(directory, module, use_unwind=False):
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
