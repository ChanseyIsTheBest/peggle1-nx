"""What the Switch compiler makes of sampled recompiled files with the rounds' build switches on (as built) and off:
instructions, loads, stores, msr fpcr, dmb, calls. Uses tools/switch_compile_check.py (no LTO, nothing in out/nx is
touched); files of one configuration are compiled in parallel.

  python tools/switch_rounds_measure.py [file numbers ...]
"""
import concurrent.futures
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__))).replace(chr(92), '/')
OBJDUMP = 'C:/devkitPro/devkitA64/bin/aarch64-none-elf-objdump.exe'
OFF = ['-DREX_RO_CONSTANTS_OFF', '-DREX_FLOAT_LOADS_INTEGER', '-DREX_LEAF_LOCALS_OFF', '-DREX_LOOP_LOCALS_OFF',
       '-DREX_TRAP_ATTR=', '-DREX_PROMOTED_CALLS_OFF', '-DREX_FLUSH_FLOW_OFF', '-DREX_GUEST_CAS_LEGACY',
       '-DREX_BSWAP128_TABLE', '-DREX_SIMDE_SHUFFLES', '-DREX_VMX_HELPERS_X86_FORM', '-fno-omit-frame-pointer',
       '-fmath-errno']
CONFIGS = {'on': [], 'off': OFF}
INS = re.compile(r'^\s+[0-9a-f]+:\s+(\S+)\s*(.*)$')


def stats(obj):
    d = subprocess.run([OBJDUMP, '-d', '--no-show-raw-insn', obj], capture_output=True, text=True).stdout
    s = dict(ins=0, loads=0, stores=0, msr=0, dmb=0, bl=0, blr=0, x29=0)
    for line in d.split('\n'):
        m = INS.match(line)
        if not m:
            continue
        op, args = m.group(1), m.group(2)
        s['ins'] += 1
        if op.startswith(('ldr', 'ldp', 'ldur', 'ldx', 'lda')):
            s['loads'] += 2 if op == 'ldp' else 1
        elif op.startswith(('str', 'stp', 'stur', 'stx', 'stl')):
            s['stores'] += 2 if op == 'stp' else 1
        elif op == 'msr' and 'fpcr' in args:
            s['msr'] += 1
        elif op == 'dmb':
            s['dmb'] += 1
        elif op == 'bl':
            s['bl'] += 1
        elif op == 'blr':
            s['blr'] += 1
        if op == 'mov' and args.startswith('x29, sp'):
            s['x29'] += 1
    return s


def compile_one(n, flags):
    src = f'generated/default/peggle1_recomp.{n}.cpp'
    cmd = [sys.executable, os.path.join(ROOT, 'tools', 'switch_compile_check.py')]
    for f in flags:
        cmd += ['--add', f]
    r = subprocess.run(cmd + [src], capture_output=True, text=True, cwd=ROOT)
    if 'ok (' not in r.stdout:
        return n, None, (r.stdout + r.stderr)[-1500:]
    obj = os.path.join(ROOT, 'out', 'switch_compile_check', re.sub(r'\W', '_', src) + '.o')
    return n, stats(obj), ''


def main():
    files = [int(a) for a in sys.argv[1:]] or [13, 31, 57, 77, 104]
    total = {}
    for name, flags in CONFIGS.items():
        # One at a time: every run of the tool copies the precompiled header's source to the same file.
        with concurrent.futures.ThreadPoolExecutor(1) as ex:
            results = list(ex.map(lambda n: compile_one(n, flags), files))
        t = None
        for n, s, err in results:
            if s is None:
                print(f'{name} file {n}: FAILED\n{err}')
                return 1
            print(f'{name:3s} recomp.{n}: ' + ', '.join(f'{k} {v}' for k, v in s.items()))
            t = s if t is None else {k: t[k] + s[k] for k in t}
        total[name] = t
        print(f'{name:3s} TOTAL of {len(files)} files: ' + ', '.join(f'{k} {v}' for k, v in t.items()))
    on, off = total['on'], total['off']
    print('change with the switches on: ' + ', '.join(
        f'{k} {100.0 * (on[k] - off[k]) / off[k]:+.1f} %' if off[k] else f'{k} {on[k]}' for k in on))
    return 0


if __name__ == '__main__':
    sys.exit(main())
