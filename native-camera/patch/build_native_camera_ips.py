"""Builds the MouseCamera-Native game patch (IPS32) for OOT on Switch 2, main build E0F3CCFAF50065AE...

    python build_native_camera_ips.py [--llvm <dir with clang/ld.lld/llvm-objcopy>] [--main <main.elf>] [--out <dir>]

What it does
  1. Compiles cave.c + hooks.S for AArch64 and links them at a fixed address in the unused space after the
     game's code (module offset 0x3C46400; the Dynamic FPS patch uses 0x3C46200-0x3C46277).
  2. Replaces two game instructions with branches into that code:
       - module 0x644C80: the first instruction of the player camera update (f_80644c80, the camera in
         x0). This is the v2 hook (Triforce's design): the mouse owns the camera's angles there.
       - module 0x0625370 `str s0, [sp, #12]`, where the right stick has just been turned into this
         frame's yaw and pitch. In v2 this hook no longer adds the mouse; it only notes that the
         stick turned the camera, so the entry hook adopts the result.
  3. Writes <out>/MouseCamera-Native/exefs/<build id>.ips (IPS32: offsets are module offset + 0x100).

--main is optional: when given (a decompressed main as an ELF at 0x80b75000, e.g. from nso2elf.py), the
script checks that the replaced instructions and the cave area are what it expects before writing.

CAM_ENTRY_INSN below is the original first instruction at 0x644C80. Until it has been read from a real
main and pinned (the --main run prints it), the script refuses to write a shippable .ips: with --dev it
writes <build id>-DEV.ips instead, which no emulator will load, for size/disassembly checks only.
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile

BUILD_ID = 'E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348'
ELF_BASE = 0x80B75000                 # address the offsets below are written against
CAVE = 0x3C46400                      # module offset of the code cave
CAVE_END = 0x3C47000                  # end of the executable segment (page end)
STATE = 0x4E9FE40                     # module offset of spare BSS for the state (Dynamic FPS uses 0x4E9FE00-0x4E9FE0F)
GET_MOUSE_STATES = 0x3C44800          # module offset of the game's PLT stub for nn::hid::GetMouseStates
CAM_ENTRY_INSN = 0xAA1F03E3   # mov x3, xzr: read from Gothan's decompressed main (build E0F3CCFA...) on 2026-11-23
HOOKS = [  # module offset, expected original instruction, symbol
    (0x0644C80, CAM_ENTRY_INSN, 'oot_hook_cam'),  # first instruction of f_80644c80 (player camera update)
    (0x0625370, 0xBD000FE0, 'oot_hook_rates'),   # str s0, [sp, #12]  (player camera: stick -> turn this frame)
]
NSO_HEADER = 0x100
HERE = os.path.dirname(os.path.abspath(__file__))


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit('command failed: ' + ' '.join(cmd) + '\n' + r.stdout + r.stderr)
    return r.stdout


def bl(src, dst):
    off = (dst - src) // 4
    if not -(1 << 25) <= off < (1 << 25):
        sys.exit(f'branch out of range: {src:#x} -> {dst:#x}')
    return 0x94000000 | (off & 0x3FFFFFF)


def symbols(nm, elf):
    out = {}
    for line in run([nm, elf]).splitlines():
        p = line.split()
        if len(p) == 3:
            out[p[2]] = int(p[0], 16)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--llvm', default=r'C:\Users\jbmid\dev\tools\llvm-22.1.8\bin')
    ap.add_argument('--main', default='')
    ap.add_argument('--out', default=os.path.join(HERE, '..'))
    ap.add_argument('--dev', action='store_true',
                    help='allow building while a hook instruction is unpinned; writes a -DEV .ips that no emulator loads')
    a = ap.parse_args()
    clang, lld = os.path.join(a.llvm, 'clang'), os.path.join(a.llvm, 'ld.lld')
    objcopy, nm = os.path.join(a.llvm, 'llvm-objcopy'), os.path.join(a.llvm, 'llvm-nm')

    with tempfile.TemporaryDirectory() as t:
        flags = ['--target=aarch64-none-elf', '-mcpu=cortex-a78', '-O2', '-ffreestanding', '-fno-builtin',
                 '-fno-stack-protector', '-fno-pic', '-fno-asynchronous-unwind-tables', '-mno-outline-atomics',
                 '-fno-common', '-Wall', '-Werror']
        run([clang] + flags + ['-c', os.path.join(HERE, 'cave.c'), '-o', os.path.join(t, 'cave.o')])
        run([clang, '--target=aarch64-none-elf', '-c', os.path.join(HERE, 'hooks.S'), '-o', os.path.join(t, 'hooks.o')])
        ld = os.path.join(t, 'link.ld')
        with open(ld, 'w') as f:
            f.write(f'''ENTRY(oot_hook_rates)
oot_mcam_state = {ELF_BASE + STATE:#x};
_ZN2nn3hid14GetMouseStatesEPNS0_10MouseStateEi = {ELF_BASE + GET_MOUSE_STATES:#x};
SECTIONS {{
  . = {ELF_BASE + CAVE:#x};
  .cave : {{ *(.text .text.*) *(.rodata .rodata.*) }}
  /DISCARD/ : {{ *(.comment) *(.note*) *(.eh_frame*) }}
}}
''')
        elf = os.path.join(t, 'cave.elf')
        run([lld, '-T', ld, '--no-pie', '-static', '-o', elf, os.path.join(t, 'hooks.o'), os.path.join(t, 'cave.o')])
        sects = run([os.path.join(a.llvm, 'llvm-readelf'), '-S', elf])
        for bad in ('.data', '.bss'):
            if f' {bad} ' in sects or f' {bad}.' in sects:
                sys.exit(f'the cave must not have {bad}: everything has to live in the code segment')
        blob_path = os.path.join(t, 'cave.bin')
        run([objcopy, '-O', 'binary', '--only-section=.cave', elf, blob_path])
        blob = open(blob_path, 'rb').read()
        sym = symbols(nm, elf)

    if CAVE + len(blob) > CAVE_END:
        sys.exit(f'cave too large: {len(blob)} bytes, room for {CAVE_END - CAVE}')
    if b'OOTMCAM1' not in blob:
        sys.exit('settings block missing from the cave')

    patches = [(CAVE, blob)]
    for off, expect, name in HOOKS:
        patches.append((off, struct.pack('<I', bl(ELF_BASE + off, sym[name]))))

    unpinned = [name for _, expect, name in HOOKS if expect is None]
    if a.main:
        img = open(a.main, 'rb').read()
        def at(off, n):  # main.elf from nso2elf: file offset 0x1000 = module offset 0
            return img[0x1000 + off:0x1000 + off + n]
        for off, expect, name in HOOKS:
            got = struct.unpack('<I', at(off, 4))[0]
            if expect is None:
                sys.exit(f'{name}: the instruction at {off:#x} is {got:#010x}. Pin it as CAM_ENTRY_INSN '
                         f'in this script (and redo it in hooks.S where the placeholder NOP is), then rebuild.')
            if got != expect:
                sys.exit(f'{name}: game instruction at {off:#x} is {got:#010x}, expected {expect:#010x} (other game version?)')
        tail = at(CAVE, CAVE_END - CAVE)
        if tail.strip(b'\0'):
            sys.exit('the cave area is not empty in this main')
        print('main checked: hook instructions and cave area match')
    if unpinned and not a.dev:
        sys.exit('unpinned hook instruction(s): ' + ', '.join(unpinned) +
                 '. Run once with --main to read the real instruction, or pass --dev for a check-only build.')

    out_dir = os.path.join(a.out, 'MouseCamera-Native', 'exefs')
    os.makedirs(out_dir, exist_ok=True)
    ips = bytearray(b'IPS32')
    for off, data in patches:
        ips += struct.pack('>IH', off + NSO_HEADER, len(data)) + data
    ips += b'EEOF'
    path = os.path.join(out_dir, BUILD_ID + ('-DEV' if unpinned else '') + '.ips')
    if unpinned:
        print('WARNING: DEV build (unpinned hook instruction); this .ips is for inspection only and must not be shipped')
    open(path, 'wb').write(ips)
    print(f'cave {len(blob)} bytes at module {CAVE:#x}; hooks: ' +
          ', '.join(f'{n} {sym[n]:#x}' for _, _, n in HOOKS))
    print('wrote', os.path.normpath(path))


if __name__ == '__main__':
    main()
