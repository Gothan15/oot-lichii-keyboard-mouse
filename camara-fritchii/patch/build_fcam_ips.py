"""Builds the FritchiiCam game patch (IPS32) for OOT, main build E0F3CCFAF50065AE...

    python build_fcam_ips.py [--llvm <dir with clang/ld.lld>] [--main <main.elf>] [--out <dir>]

The game half of the from-scratch mouse camera for the patched Fritchii (see the Fritchii fork,
branch camara-raton: its mouse_camera mode captures the host mouse and feeds the emulated HID
mouse true relative motion). This patch only adds the mouse's turn to the camera:

  1. Compiles cave_fcam.c + hooks_fcam.S for AArch64 and links them at module offset 0x3C46400,
     in the unused space after the game's code. The cave is kept below 0x3C46C00 so the SprintB
     patch (cave at 0x3C46C00) can be installed at the same time.
  2. Replaces one game instruction with a branch into that code:
       - module 0x625370 `str s0, [sp, #12]`, in the player camera update, right after the game
         turned the right stick into this frame's yaw (s9) and pitch (s0) changes, in degrees.
         The cave adds the mouse's turn (from nn::hid::GetMouseStates deltas) to both, and the
         game applies the sum through its own "turned by hand" path.
  3. Writes <out>/FritchiiCam/exefs/<build id>.ips (IPS32: offsets are module offset + 0x100).

This patch replaces the old native camera patch (they hook the same instruction); do not install
both. --main is optional: when given (a decompressed main as an ELF at 0x80b75000, e.g. from
nso2elf.py), the script checks the replaced instruction and the cave area before writing. The
expected instruction was read from Gothan's own decompressed main (2026-11-23) and is pinned
here; a --main run against another build stops instead of shipping a patch for the wrong game.
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile

BUILD_ID = 'E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348'
ELF_BASE = 0x80B75000                 # address the offsets below are written against
CAVE = 0x3C46400                      # module offset of the code cave (the old camera patch's spot)
CAVE_END = 0x3C46C00                  # the SprintB cave starts here: stay below it
STATE = 0x4E9FE40                     # module offset of spare BSS for the state (the old camera's spot)
GET_MOUSE_STATES = 0x3C44800          # module offset of the game's PLT stub for nn::hid::GetMouseStates
HOOKS = [  # module offset, expected original instruction, symbol
    (0x625370, 0xBD000FE0, 'oot_hook_fcam_rates'),  # str s0, [sp, #12]  (this frame's stick turn)
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
    a = ap.parse_args()
    clang, lld = os.path.join(a.llvm, 'clang'), os.path.join(a.llvm, 'ld.lld')
    objcopy, nm = os.path.join(a.llvm, 'llvm-objcopy'), os.path.join(a.llvm, 'llvm-nm')

    with tempfile.TemporaryDirectory() as t:
        flags = ['--target=aarch64-none-elf', '-mcpu=cortex-a78', '-O2', '-ffreestanding', '-fno-builtin',
                 '-fno-stack-protector', '-fno-pic', '-fno-asynchronous-unwind-tables', '-mno-outline-atomics',
                 '-fno-common', '-Wall', '-Werror']
        run([clang] + flags + ['-c', os.path.join(HERE, 'cave_fcam.c'), '-o', os.path.join(t, 'cave_fcam.o')])
        run([clang, '--target=aarch64-none-elf', '-c', os.path.join(HERE, 'hooks_fcam.S'), '-o', os.path.join(t, 'hooks_fcam.o')])
        ld = os.path.join(t, 'link.ld')
        with open(ld, 'w') as f:
            f.write(f'''ENTRY(oot_hook_fcam_rates)
oot_fcam_state = {ELF_BASE + STATE:#x};
_ZN2nn3hid14GetMouseStatesEPNS0_10MouseStateEi = {ELF_BASE + GET_MOUSE_STATES:#x};
SECTIONS {{
  . = {ELF_BASE + CAVE:#x};
  .cave : {{ *(.text .text.*) *(.rodata .rodata.*) }}
  /DISCARD/ : {{ *(.comment) *(.note*) *(.eh_frame*) }}
}}
''')
        elf = os.path.join(t, 'cave.elf')
        run([lld, '-T', ld, '--no-pie', '-static', '-o', elf, os.path.join(t, 'hooks_fcam.o'), os.path.join(t, 'cave_fcam.o')])
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
    if b'OOTFCAM1' not in blob:
        sys.exit('settings block missing from the cave')

    patches = [(CAVE, blob)]
    for off, expect, name in HOOKS:
        patches.append((off, struct.pack('<I', bl(ELF_BASE + off, sym[name]))))

    if a.main:
        img = open(a.main, 'rb').read()
        def at(off, n):  # main.elf from nso2elf: file offset 0x1000 = module offset 0
            return img[0x1000 + off:0x1000 + off + n]
        for off, expect, name in HOOKS:
            got = struct.unpack('<I', at(off, 4))[0]
            if got != expect:
                sys.exit(f'{name}: game instruction at {off:#x} is {got:#010x}, expected {expect:#010x} (other game version?)')
        tail = at(CAVE, CAVE_END - CAVE)
        if tail.strip(b'\0'):
            sys.exit('the cave area is not empty in this main')
        print('main checked: hook instruction and cave area match')

    out_dir = os.path.join(a.out, 'FritchiiCam', 'exefs')
    os.makedirs(out_dir, exist_ok=True)
    ips = bytearray(b'IPS32')
    for off, data in patches:
        ips += struct.pack('>IH', off + NSO_HEADER, len(data)) + data
    ips += b'EEOF'
    path = os.path.join(out_dir, BUILD_ID + '.ips')
    open(path, 'wb').write(ips)
    print(f'cave {len(blob)} bytes at module {CAVE:#x}; hooks: ' +
          ', '.join(f'{n} {sym[n]:#x}' for _, _, n in HOOKS))
    print('wrote', os.path.normpath(path))


if __name__ == '__main__':
    main()
