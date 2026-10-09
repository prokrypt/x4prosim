# ESP32-C3 CPU timing

The RISC-V translator can charge instruction-dependent virtual-time ticks under
`-icount`. Configure the CPU before realization, for example:

```
-machine x3 -icount shift=2,sleep=on \
-global espressif-riscv-cpu.cost-flash-fetch=2 \
-global espressif-riscv-cpu.cost-div=34
```

All seven properties are unsigned integers from 0 to 255. `cost-base` defaults
to 1 (zero is treated as 1 so time always advances); `cost-load`, `cost-store`,
`cost-mul`, `cost-div`, `cost-branch`, and `cost-flash-fetch` default to zero.
The instruction cost is base plus its class surcharge plus its fetch surcharge.
At shift=2 a tick is 4 ns; these are effective calibration units, not literal
160 MHz hardware cycles. No rebuild is needed to change the fit. Qdev rejects
changes after realization: restart the emulator to change these properties.

The class decoder covers RV32IMC integer loads/stores, all M-extension multiply
and divide/remainder variants, conditional branches, JAL/JALR, and compressed
loads/stores/branches/jumps. Conditional branches pay the same surcharge whether
taken or not. Fetch adds a surcharge for each instruction starting in
0x42000000–0x427fffff. ROM and IRAM incur no fetch surcharge. This averages flash
cache behavior; it does not model a 16 KB cache, misses, alignment, data caching,
branch prediction, operand-dependent divide latency, or instruction overlap.
Other RISC-V extensions are outside this C3 model.

`TranslatorOps.insn_cost` in `include/exec/translator.h` is optional; targets
without it pay one tick. `accel/tcg/translator.c` sums the costs into
`tb->icount_cost` and patches the entry decrement. `tb->icount` remains the
number of guest instructions, as do `CF_COUNT_MASK`, plugin instruction counts,
and unwind-table row counts. RISC-V's `tcg_set_insn_start_param` arguments remain
fault metadata; they are not repurposed as time costs.

A separate cost accompanies each encoded unwind row. Exception recovery and
MMIO recompilation refund ticks from the faulting instruction onward. When a
remaining timer budget cannot fit a block, `tb_insns_for_ticks()` converts ticks
back to an instruction limit. If it cannot fit even one instruction, a single-instruction TB runs with
interrupt checks deferred. A borrow into the signed `icount_extra` preserves
its full cost when the 16-bit decrementer wraps; unwind reverses that borrow
when necessary. The resulting timer interrupt is late by less than one
instruction's cost, and short timer intervals cannot starve execution. RISC-V limits the TB instruction count using the worst-case cost and
saturates individual costs at 65535, so the 16-bit decrement cannot overflow.
Xtensa retains one tick per instruction.

Weighted costs are intended for deterministic simulation with ordinary icount.
Record/replay, migration across different cost settings, and non-C3 RISC-V
extension timing have not been validated. Hardware calibration is a separate
step using the CPU_PROBE measurements.

## Emulator regression test

Run the small RV32IMC test on QEMU's `virt` machine (no ESP firmware required):

```
python3 x4prosim/sdcal/cpu-evidence/verify.py \
  build/qemu-system-riscv32 /path/to/riscv32-esp-elf-gcc
```

The test measures `mtime` around 10,000-iteration loops containing arithmetic,
loads/stores, multiply, divide/remainder, and compressed loads/stores. It checks
each surcharge independently, verifies flash cost does not apply outside the
C3 window, repeats divide timing with one-instruction TBs, and exercises all
properties at 255. A programmed timer deadline forces the uninterruptible
single-instruction path; the test verifies both its execution trace and full
elapsed cost. MMIO timer reads also exercise unwind/recompilation accounting.
[instruction-results.json](cpu-evidence/instruction-results.json) records the
measured 100 ns `mtime` ticks; the normal loop is 1200 ticks, load cost 3 raises
only load loops to 2400, and divide cost 34 raises divide/remainder to 14800.
