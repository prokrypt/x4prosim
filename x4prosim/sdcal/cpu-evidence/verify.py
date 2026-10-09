#!/usr/bin/env python3
"""Check actual virtual time on RV32IMC, including MMIO unwind and short TBs.

Usage: verify.py path/to/qemu-system-riscv32 path/to/riscv32-esp-elf-gcc
No device, CrossPoint build, Python packages, or writable checkout required.
"""
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

qemu, compiler = map(lambda p: str(Path(p).resolve()), sys.argv[1:3])
source = Path(__file__).with_name('instruction-costs.S')
with tempfile.TemporaryDirectory(prefix='qemu-cpu-cost-') as tmp:
    elf = str(Path(tmp) / 'test.elf')
    subprocess.run([compiler, '-march=rv32imc', '-mabi=ilp32', '-nostdlib',
                    '-Wl,-Ttext=0x80000000', '-Wl,--no-relax',
                    '-o', elf, str(source)], check=True)
    cmd = [qemu, '-M', 'virt', '-bios', 'none', '-kernel', elf,
           '-icount', 'shift=2,sleep=off', '-nographic']
    rows = {}

    def run(name, args, expected, tolerance=1):
        result = subprocess.run(cmd + args, capture_output=True,
                                check=True, timeout=30)
        values = struct.unpack('<8I', result.stdout)
        assert all(abs(a - b) <= tolerance for a, b in zip(values, expected)), (
            name, values, expected, result.stderr.decode())
        rows[name] = values

    run('default', [], [1200] * 8)
    # Each interval executes 10,000 (operation, decrement, branch) iterations.
    # virt's mtime tick is 100 ns; icount shift=2 makes each cost tick 4 ns.
    indices = {'load': [1, 6], 'store': [2, 7], 'mul': [3], 'div': [4, 5],
               'branch': list(range(8)), 'flash-fetch': []}
    for prop, affected in indices.items():
        expected = [2400 if i in affected else 1200 for i in range(8)]
        run(prop, ['-global', f'riscv-cpu.cost-{prop}=3'], expected)
    run('base', ['-global', 'riscv-cpu.cost-base=3'], [3600] * 8)
    run('single-insn-tbs', ['-accel', 'tcg,one-insn-per-tb=on',
                           '-global', 'riscv-cpu.cost-div=34'],
        [1200, 1200, 1200, 1200, 14800, 14800, 1200, 1200])
    args = []
    for prop in ['base', *indices]:
        args.extend(['-global', f'riscv-cpu.cost-{prop}=255'])
    run('maximum-costs', args, [408000] + [510000] * 7, tolerance=25)
    # Arrange a timer deadline inside a maximum-cost instruction. Execution
    # must progress and retain the full cost, including the six setup insns.
    deadline_source = Path(tmp) / 'deadline.S'
    deadline_source.write_text(source.read_text().replace(' lw t2, 0(s0)',
        ' lw t2, 0(s0)\n li t4, 0x2004000\n li t5, -1\n'
        ' sw t5, 4(t4)\n addi t5, t2, 100\n sw t5, 0(t4)\n'
        ' sw zero, 4(t4)'))
    subprocess.run([compiler, '-march=rv32imc', '-mabi=ilp32', '-nostdlib',
                    '-Wl,-Ttext=0x80000000', '-Wl,--no-relax',
                    '-o', elf, str(deadline_source)], check=True)
    trace = Path(tmp) / 'deadline.trace'
    run('deadline-overrun', args + ['-d', 'exec', '-D', str(trace)],
        [value + 92 for value in rows['maximum-costs']], tolerance=1)
    overrun_tbs = sum(bool(int(line.split(']')[0].rsplit('/', 1)[1], 16) &
                           0x10000) for line in trace.read_text().splitlines()
                      if line.startswith('Trace '))
    assert overrun_tbs > 0, 'deadline did not exercise CF_NOIRQ overrun path'
    rows['deadline_overrun_tbs'] = overrun_tbs
    print(json.dumps(rows, indent=2))
