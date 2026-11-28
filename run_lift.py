#!/usr/bin/env python3
"""Galactic Battlegrounds lift driver: Battlegrounds.exe -> src/recomp/gen/.

Battlegrounds.exe (v1.1) is the whole game (MSVC6 C++, DirectDraw, base
0x00400000, no relocations), catalogued by disasm32 into work/functions.json.
The driver is Crazy Taxi's, plus MSVC exception frames: every function that
installs a C++ try or a __try gets a landing pad (pcrecomp tools/lift/eh32.py,
docs/exceptions.md).

    py -3 run_lift.py --all
    py -3 run_lift.py --roots 0x00632881
"""
import argparse
import json
import os
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
# PCRECOMP picks the toolkit checkout, the same knob CMakeLists.txt has: the
# lifter and the runtime header must come from the same tree.
_TOOLS = os.path.join(os.environ.get('PCRECOMP', os.path.join(_HERE, '..', 'pcrecomp')), 'tools')
sys.path.insert(0, os.path.join(_TOOLS, 'lift'))
sys.path.insert(0, os.path.join(_TOOLS, 'pe'))

from capstone import Cs, CS_ARCH_X86, CS_MODE_32          # noqa: E402
from generate import (EXTENT_REACH, closure, find_splits, true_extent,  # noqa: E402
                      linear_disassemble_function, lift_function_linear, write_chunk)
from lift32 import Lifter                                  # noqa: E402
import eh32                                                # noqa: E402
from pe_analyze import analyze_pe, build_iat_map           # noqa: E402

OUT = os.path.join(_HERE, 'src', 'recomp', 'gen')
STATS = os.path.join(_HERE, 'work', 'lift_stats.json')

# (file, catalog, PE timestamp of the one build supported, what it is).
# A different build lifts, but the patches and seeds are at this build's
# addresses: it would crash in the game, so it is refused (README).
MODULES = [
    (os.path.join(_HERE, 'work', 'Battlegrounds.exe'), os.path.join(_HERE, 'work', 'functions.json'),
     0x3C03FD69, 'Battlegrounds.exe v1.1, from LucasArts BGUpdate1_1.exe (2001-11-28)'),
]

# Entries the catalog does not find: 0x00427090 (called through a pointer
# from 0x00424425), and the THIS_COD blitter's jump-table targets (tables at
# 0x0064C300-0x0064E780, read from the image; a skirmish map's first frame
# jumped to 0x00646465, not a function entry).
RUN_SEEDS = [
    0x00427090,
    0x00646465,
    0x0064EE80,
    0x0064EEA0,
    0x0064EEA5,
    0x0064EF40,
    0x0064EF51,
    0x0064EF80,
    0x0064F020,
    0x0064F0C0,
    0x0064F160,
    0x0064F3E0,
    0x0064F400,
    0x0064F420,
    0x0064F460,
    0x0064F46B,
    0x0064F480,
    0x0064F4C0,
    0x0064F500,
    0x0064F540,
    0x0064F580,
    0x0064F596,
    0x0064F5C0,
    0x0064F5D6,
    0x0064F640,
    0x0064F660,
    0x0064F680,
    0x0064F6A0,
    0x00650080,
    0x00650111,
    0x00650117,
    0x0065011B,
    0x00650124,
    0x0065012B,
    0x00650136,
    0x00650140,
    0x00650149,
    0x00650155,
    0x0065015B,
    0x00650162,
    0x00650168,
    0x0065016E,
    0x00650172,
    0x0065017B,
]

DATA_IN_CODE = []


def is_data(va):
    return any(lo <= va < hi for lo, hi in DATA_IN_CODE)

# C run after the instruction at the address. The lift stays faithful: a
# patch is a line of C for something the original only got away with on real
# DirectDraw, or the remaster's, calling the host (src/runtime/hires.c).
PATCHES = {
    # The mouse pointer's "center" (sub_0049F5C0) reads the draw area the
    # pointer is suspended from across a mode change (sub_0049E540 clears it;
    # the next main-loop tick, sub_00428F80, puts it back). Starting a game at
    # 1024x768 or 1280x1024 changes the mode and centers at once: a null read.
    # Real DirectDraw's SetDisplayMode let Windows' messages resume it first.
    0x0049F5C0: 'if (!eax) { RECOMP_FLAGS_OUT(); esp += 4; RECOMP_REGS_OUT(); return; } '
                '/* fix: pointer suspended, run_lift.py */',
    # ---- high resolution and widescreen (hires.c, docs/hires.md) ----
    # The size the game enters (sub_0042C0C0; w, h in eax, esi): the
    # remaster's when one is set.
    0x0042C0D3: 'swgb_game_res(&eax, &esi); /* remaster: resolution, hires.c */',
    # TPanel's two rect setters, after their first read: the in-game screen's
    # panels moved from the class layout to the real size. set_rect(x1, y1,
    # x2, y2) (edx = y2) calls the class's set_rect(x, y, w, h) (eax = x),
    # which is also called straight; hires.c says why both.
    0x004B5780: 'swgb_rect(ecx, esp); edx = MEM32(esp + 0x10); /* remaster: widescreen layout, hires.c */',
    0x004B579E: 'swgb_rect_done(); /* remaster: widescreen layout, hires.c */',
    0x004B5660: 'swgb_rect_xywh(ecx, esp); eax = MEM32(esp + 4); /* remaster: widescreen layout, hires.c */',
    # The DRS read (fastcall type, id, &allocated, &size; ret 8), on entry:
    # the bottom panel's picture, widened.
    0x004D4B30: 'if (swgb_fetch(ecx, edx, MEM32(esp + 0xC), MEM32(esp + 0x10), &eax)) { RECOMP_FLAGS_OUT(); '
                'esp += 8 + 4 + 8; RECOMP_REGS_OUT(); return; } /* remaster: widescreen panel, hires.c */',
    # The screen's draw (sub_004FEDB0) puts one picture by hand, at
    # 684,982 (1280x1024) or 521,733 (1024x768): moved like the panels.
    0x004FEF67: 'swgb_xy(esp, esp + 4); /* remaster: widescreen layout, hires.c */',
    0x004FEF8F: 'swgb_xy(esp, esp + 4); /* remaster: widescreen layout, hires.c */',
}

# Size compares that pick a layout (hires.c swgb_cls): the remaster's width
# or height reads as its class's (1280x1024 or 1024x768), so the game lays
# out and draws for that class; everything else sees the real size.
# 'w': a width, 'h': a height. The patch goes after the cmp's flag lines.
CLASS_SITES = {
    0x004F5314: 'w', 0x004F531B: 'w',                      # the screen's layout column (screen_coords.txt)
    0x004FC67E: 'w', 0x004FC685: 'h', 0x004FC6AC: 'w',     # its panel picture (51101-51146; 1600x1200 has none)
    0x004FC6B3: 'h', 0x004FC6F4: 'w', 0x004FC6FB: 'h',
    0x004FEF27: 'w', 0x004FEF54: 'w', 0x004FEF7C: 'w',     # its draw
    0x005013C1: 'w', 0x005013D4: 'w',                      # its sub-panels' places
    0x005D4988: 'w', 0x005D499E: 'w',                      # the resources' and unit display's size index
    0x005D5192: 'w', 0x005D51A5: 'w', 0x005D5D39: 'w', 0x005D5D48: 'w',
    0x004B7916: 'w', 0x004B78F1: 'w',                      # text: the fonts for the size
    0x00469E84: 'w', 0x00469E8C: 'w', 0x00469E94: 'w',     # the map view's draw
    0x00462D58: 'h', 0x00462EF2: 'h', 0x00462FCA: 'h',     # the map view's setup
    0x00463446: 'h', 0x00463454: 'h', 0x0046346E: 'h',
}
AFTER_BLOCK = {va: '_flag_a = swgb_cls(0x%08Xu, _flag_a, %d); _cf = (uint32_t)CMP_B(_flag_a, _flag_b); '
                   '/* remaster: size class, hires.c */' % (va, k == 'h') for va, k in CLASS_SITES.items()}

# Functions the host does instead (src/runtime/*.c says why for each).
HLE = {
    0x0062343B: 'swgb_inflate',   # the deflate decoder, a stack-copying coroutine (inflate.c)
    0x00477DA0: 'swgb_dplay_ok',  # the DirectPlay version check: dplay32 is there (host.c)
}
# The host functions the patches call (hires.c).
HOOK_DECLS = [
    'void swgb_game_res(uint32_t* w, uint32_t* h)',
    'void swgb_rect(uint32_t self, uint32_t esp)',
    'void swgb_rect_done(void)',
    'void swgb_rect_xywh(uint32_t self, uint32_t esp)',
    'int swgb_fetch(uint32_t type, uint32_t id, uint32_t allocated, uint32_t size, uint32_t* data)',
    'void swgb_xy(uint32_t px, uint32_t py)',
    'uint32_t swgb_cls(uint32_t site, uint32_t v, int is_h)',
]
PATCH_DECLS = ''.join('void %s(void);\n' % f for f in HLE.values()) + ''.join(d + ';\n' for d in HOOK_DECLS)


def apply_patches(body, patches, after_block=False):
    """Add each patch's C after the lines that lift its instruction.

    Some instructions lift to a block over several lines (idiv's `{ ... }`):
    the patch goes after the block closes, not inside it. From the Tiberium
    Wars driver (tiberiumwars-recomp run_lift.py). after_block: after every
    line the instruction lifts to, up to the next instruction or label (a
    cmp's comment comes first and its flag lines after it)."""
    for va, c in patches.items():
        tag = '/* 0x%08X:' % va
        k = body.find(tag)
        if k != -1:
            s = body.rfind('\n', 0, k) + 1
            e = body.index('\n', k)
            depth = body[s:e].count('{') - body[s:e].count('}')
            while depth > 0:
                s, e = e + 1, body.index('\n', e + 1)
                depth += body[s:e].count('{') - body[s:e].count('}')
            while after_block:
                n = body.index('\n', e + 1)
                line = body[e + 1:n]
                if '/* 0x' in line or line.startswith('L_') or line.strip() == '}':
                    break
                e = n
            body = body[:e + 1] + '    ' + c + '\n' + body[e + 1:]
    return body


def load_seeds(path):
    """Addresses from a seed file: a list of {'address': va} or of plain VAs."""
    if not path or not os.path.exists(path):
        return set()
    return {e['address'] if isinstance(e, dict) else e for e in json.load(open(path))}


def exe_stamp(path):
    """The PE header's TimeDateStamp: which build of a module this is."""
    with open(path, 'rb') as f:
        head = f.read(4096)
    nt = int.from_bytes(head[0x3C:0x40], 'little')
    return int.from_bytes(head[nt + 8:nt + 12], 'little')


class Out:
    """The generated tree: chunks numbered across both modules."""

    def __init__(self, path, split):
        self.path, self.split = path, split
        self.chunk, self.idx, self.entries = [], 0, []

    def add(self, body, addr, name):
        self.chunk.append((body, addr, name))
        self.entries.append((addr, name))
        if len(self.chunk) >= self.split:
            self.flush()

    def flush(self):
        if self.chunk:
            write_chunk(self.path, self.idx, self.chunk)
            self.idx += 1
            self.chunk = []


def lift_module(path, catalog, args, out, extra_roots):
    """Lift one module into `out`. Returns (entry VA, stats)."""
    info = analyze_pe(path)
    iat = build_iat_map(info)
    cs, ce = info.code_start, info.code_end
    print('[*] %s: base=0x%08X code=0x%08X-0x%08X IAT=%d'
          % (os.path.basename(path), info.image_base, cs, ce, len(iat)))

    # The code section, by address.
    cat = json.load(open(catalog))
    byaddr = {f['address']: f for f in cat['functions'] if cs <= f['address'] < ce and not is_data(f['address'])}
    print('[*]   catalog: %d functions inside the code section' % len(byaddr))

    raw = open(path, 'rb').read()
    # The code range spans sections here (THIS_COD after .text: matrix and
    # blit routines), so every section is laid out at its address. Read from
    # .text alone, a function in the second section lifted to an empty body
    # that never popped its return address.
    code = bytearray(ce - cs)
    for sec in info.sections:
        lo = info.image_base + sec.virtual_address
        a, b = max(lo, cs), min(lo + sec.raw_size, ce)
        if a < b:
            code[a - cs:b - cs] = raw[sec.raw_offset + a - lo:sec.raw_offset + b - lo]
    code = bytes(code)

    def read(va, n):
        """n bytes of the image at va (FuncInfo and scope tables live in .rdata)."""
        for sec in info.sections:
            lo = info.image_base + sec.virtual_address
            if lo <= va and va + n <= lo + sec.raw_size:
                return raw[sec.raw_offset + va - lo:sec.raw_offset + va - lo + n]
        return None

    # Bound each injected entry by the next known one: an entry landing
    # mid-code handed `ce` makes the extent walk descend the whole section.
    want = {a for a in set(RUN_SEEDS) | load_seeds(args.seeds) if cs <= a < ce and a not in byaddr}
    known = sorted(set(byaddr) | want)
    nxt = {a: (known[i + 1] if i + 1 < len(known) else ce) for i, a in enumerate(known)}
    for a in want:
        byaddr[a] = {'address': a, 'end': min(nxt[a], a + 0x100), 'calls_to': [], 'entry_kind': 'start'}

    entry = info.image_base + info.entry_point_rva
    roots = [entry] + [info.image_base + e.rva for e in getattr(info, 'exports', [])
                       if getattr(e, 'rva', 0)] + [r for r in extra_roots if cs <= r < ce]
    chosen = set(byaddr) if args.all else set(closure(byaddr, roots, args.max))
    print('[*]   lifting %d of %d functions (%s)'
          % (len(chosen), len(byaddr), 'all' if args.all else 'closure, cap %d' % args.max))

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True

    # MSVC exception frames (eh32.frame). Where a handler continues inside the
    # function that installed the frame (an __except block, a catch's
    # continuation) is a label in that body, never an entry of its own:
    # disasm32 takes a scope table's __except pointers for functions, and
    # dispatched as one an __except block would run as a call and "return"
    # into _except_handler3.
    frames = {}
    for a in sorted(byaddr):
        head = list(md.disasm(code[a - cs:a - cs + 64], a, 12))
        if not any(i.op_str == 'dword ptr fs:[0], esp' for i in head):
            continue
        reached = set()
        end, _ = true_extent(md, code, cs, a, min(a + EXTENT_REACH, ce), set(byaddr), reached=reached)
        insns, _ = linear_disassemble_function(md, code, cs, a, end, reached=reached)
        fr = eh32.frame(md, read, insns)
        if fr and fr[0]:
            frames[a] = fr
    resume = set().union(*(r for r, _ in frames.values())) if frames else set()
    dropped = resume & set(byaddr)
    for a in dropped:
        del byaddr[a]
    chosen -= dropped
    print('[*]   %d functions install an exception frame with somewhere to continue '
          '(%d frame-pointer-omitted); %d resume points, %d were catalog entries'
          % (len(frames), sum(1 for _, o in frames.values() if o == 0), len(resume), len(dropped)))

    # precise carry: adc/sbb take their carry from the flag state that set it.
    # The default reads a `_cf` that add/sub/cmp never write, which breaks every
    # 64-bit add and subtract (The Movies lost its menu video to it).
    lifter = Lifter(iat_map=iat, lifted=set(byaddr), precise_carry=True, precise_sbb=True)
    ordered = sorted(byaddr)
    starts = {a for a in ordered if byaddr[a].get('entry_kind') != 'alias'}
    splits = find_splits(md, code, cs, ce, starts)
    starts -= splits          # still dispatchable; just not a wall inside its parent
    errors = dirty = 0

    def lift_one(addr, name, end, reached):
        nonlocal errors
        try:
            lo = min(reached) if reached else addr   # a chunk can sit below the entry
            insns, leaders = (linear_disassemble_function(md, code, cs, lo, end, reached=reached)
                              if end > addr else ([], None))
            if leaders is not None:
                leaders.add(addr)
            resume, off = frames.get(addr, (None, None))
            if addr in HLE:
                out.add('void %s(void) { %s(); }\n' % (name, HLE[addr]), addr, name)
                return
            if not insns:
                raise ValueError('no instructions at 0x%08X' % addr)
            body = lift_function_linear(lifter, name, insns, leaders, addr, eh_resume=resume, eh_off=off)
            body = apply_patches(body, {va: c for va, c in PATCHES.items() if reached and va in reached})
            body = apply_patches(body, {va: c for va, c in AFTER_BLOCK.items() if reached and va in reached},
                                 after_block=True)
        except Exception as e:                      # noqa: BLE001 -- counted, not hidden
            body = '/* ERROR %s: %s */\nvoid %s(void) { }\n' % (name, e, name)
            errors += 1
        out.add(body, addr, name)

    # A direct branch or call to an address nothing catalogued is a tail call
    # or a jump into shared code the catalog missed. The target has to be
    # dispatchable or the RECOMP_ITAIL cannot resolve, so each round's outside
    # targets are lifted in the next round, until none are new.
    todo, added = sorted(chosen), 0
    while todo:
        outside = set()
        for addr in todo:
            reached, behind, called = set(), set(), set()
            end, clean = true_extent(md, code, cs, addr, min(addr + EXTENT_REACH, ce), starts,
                                     reached=reached, behind=behind, called=called,
                                     seeds=frames.get(addr, ((),))[0])
            outside |= {t for t in behind if t not in reached} | called
            dirty += not clean
            lift_one(addr, 'sub_%08X' % addr, end, reached)
        todo = sorted(t for t in outside if cs <= t < ce and t not in byaddr and not is_data(t))
        for t in todo:
            byaddr[t] = {'address': t, 'end': ce, 'calls_to': [], 'entry_kind': 'start'}
            chosen.add(t)
        added += len(todo)

    stubs = [a for a in ordered if a not in chosen]
    for a in stubs:
        name = 'sub_%08X' % a
        out.add('void %s(void) { RECOMP_NOT_LIFTED(0x%08Xu); }\n' % (name, a), a, name)
    print('[*]   lifted %d, %d outside targets added, %d stubs, %d errors, %d with no terminator'
          % (len(chosen), added, len(stubs), errors, dirty))
    return entry, {'lifted': len(chosen), 'stubs': len(stubs), 'errors': errors, 'no_terminator': dirty}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=OUT)
    ap.add_argument('--roots', default='', help='comma-separated extra root VAs')
    ap.add_argument('--max', type=int, default=3000, help='closure size cap')
    ap.add_argument('--all', action='store_true', help='lift every function')
    ap.add_argument('--split', type=int, default=400, help='functions per .c file')
    ap.add_argument('--seeds', default=os.path.join(_HERE, 'work', 'run_seeds.json'),
                    help='seed_from_log.py JSON of unresolved targets from runs')
    args = ap.parse_args()
    for path, catalog, stamp, what in MODULES:
        if not os.path.exists(path):
            sys.exit('no %s -- README, Step by step' % path)
        if not os.path.exists(catalog):
            sys.exit('no catalog at %s -- run disasm32.py first (README, Step by step)' % catalog)
        if exe_stamp(path) != stamp:
            sys.exit('%s is not the build this project supports: its PE timestamp is 0x%08X, '
                     'expected 0x%08X: %s (README).' % (path, exe_stamp(path), stamp, what))

    os.makedirs(args.out, exist_ok=True)
    for fn in os.listdir(args.out):                 # a smaller lift must not leave stale chunks
        if fn.startswith('recomp_') and fn.endswith('.c'):
            os.remove(os.path.join(args.out, fn))
    t0 = time.time()
    out = Out(args.out, args.split)
    roots = [int(x, 0) for x in args.roots.split(',') if x.strip()]
    entries_va, stats = [], {'lifted': 0, 'stubs': 0, 'errors': 0, 'no_terminator': 0}
    for path, catalog, _, _ in MODULES:
        entry, s = lift_module(path, catalog, args, out, roots)
        entries_va.append(entry)
        for k in stats:
            stats[k] += s[k]
    out.flush()

    with open(os.path.join(args.out, 'recomp_funcs.h'), 'w', newline='\n') as f:
        f.write('/* Battlegrounds.exe - AUTO-GENERATED by run_lift.py */\n#pragma once\n'
                '#include <stdint.h>\n\n'
                'void recomp_not_lifted(uint32_t va);\n'
                '#define RECOMP_NOT_LIFTED(va) recomp_not_lifted(va)\n\n' + PATCH_DECLS + '\n')
        for a, n in out.entries:
            f.write('void %s(void);\n' % n)
    with open(os.path.join(args.out, 'recomp_dispatch.c'), 'w', newline='\n') as f:
        f.write('/* Battlegrounds.exe - AUTO-GENERATED by run_lift.py */\n'
                '#include "recomp_types.h"\n#include "recomp_funcs.h"\n\n'
                'const recomp_dispatch_entry_t recomp_dispatch_table[] = {\n')
        for a, n in sorted(out.entries):
            f.write('    { 0x%08Xu, %s },\n' % (a, n))
        f.write('};\nconst uint32_t recomp_dispatch_count = %d;\n'
                'const uint32_t swgb_entry_va = 0x%08Xu;\n'
                % (len(out.entries), entries_va[0]))

    # A patch that found no instruction to follow is a fix that silently isn't there.
    gen = ''.join(open(os.path.join(args.out, fn), encoding='utf-8', errors='replace').read()
                  for fn in os.listdir(args.out) if fn.endswith('.c'))
    for va, c in list(PATCHES.items()) + list(AFTER_BLOCK.items()):
        if c not in gen:
            sys.exit('patch at 0x%08X (%s) was not applied: is 0x%08X lifted?' % (va, c, va))
    print('[*] patches applied: %d' % (len(PATCHES) + len(AFTER_BLOCK)))

    lines = sum(sum(1 for _ in open(os.path.join(args.out, fn), encoding='utf-8', errors='replace'))
                for fn in os.listdir(args.out))
    stats.update({'files': out.idx, 'lines': lines})
    json.dump(stats, open(STATS, 'w'), indent=1)
    print('=' * 60)
    print('  lifted %(lifted)d   not-lifted stubs %(stubs)d   errors %(errors)d   '
          'no terminator %(no_terminator)d' % stats)
    print('  %s lines of C in %d files, %.1fs' % (format(lines, ','), out.idx, time.time() - t0))
    print('=' * 60)


if __name__ == '__main__':
    main()
