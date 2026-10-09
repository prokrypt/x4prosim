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
saturates individual costs at 65535, so each TB cost fits in 16 bits.
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

## Configured CPU clock

`hw/riscv/esp32c3_clk.c` now stores writes to `SYSTEM_CPU_PER_CONF` (0x600c0008)
and `SYSTEM_SYSCLK_CONF` (0x600c0058). The C3 board links the clock to its CPU.
PLL selections 0 and 1 mean 80 and 160 MHz; XTAL uses the board's 40 MHz source
divided by `PRE_DIV_CNT + 1`. RC_FAST uses an approximate 17.5 MHz source with
the same divider. The integer cost multiplier is `ceil(160 MHz / CPU Hz)`:
160/80/40/10 MHz give factors 1/2/4/16. Non-integral RC_FAST ratios round upward.
The original register-model reset state is retained: PLL at 80 MHz, factor 2
(`SYSCLK_CONF=0x000a8401`, `CPU_PER_CONF=0`). This is an existing simplification
of ROM startup, which does not model the real chip's initial XTAL phase.

When the factor changes, `tb_flush()` invalidates cached translations and
`CPU_INTERRUPT_EXITTB` returns execution to the dispatcher. Ordinary register
writes with an unchanged factor do not flush. Enable clock traces with:

```
-trace enable=esp32c3_cpu_clock -D cpu-clock.trace
```

The frequency-field interpretation follows Espressif's
[ESP-IDF C3 clock implementation](https://github.com/espressif/esp-idf/blob/v5.5/components/hal/esp32c3/include/hal/clk_tree_ll.h).
This changes CPU instruction time only; peripheral clock trees and the existing
Espressif cycle-counter implementation are not recalibrated here. A final CPU
cost fit still requires the separate CPU_PROBE device table.

## CrossPoint boot validation

Booted a copy of
`/private/tmp/crosspoint-qemu-serial-cand/qemu-serial-evidence.local/builds/cand/x3-serial.bin`,
with a private 16384 MB MBR/FAT32 card made by the supplied `mksd16.py`, using
`mkfs.vfat -s 16` and a copy of the supplied `emu-card/tree`. Each run used a
fresh flash copy and an SD snapshot; no device or other checkout was modified.
The common emulator options were:

```
-L pc-bios -machine x3 -icount shift=2,sleep=on \
-drive file=flash.run,if=mtd,format=raw \
-drive file=sd.img,if=sd,format=raw,snapshot=on \
-chardev file,id=cdc,path=boot.log -serial null \
-global driver=misc.esp32s3.usb_serial_jtag,property=chardev,value=cdc \
-display none -monitor none -nic user,model=esp32_wifi
```

The saved panel executable was built from the unchanged `serialx/panel-timing`
HEAD. Firmware-side `[ms]` stamps:

| Run | SD detected | Home | SD → Home | Low-power log |
| --- | ---: | ---: | ---: | ---: |
| Panel baseline, first | 521 | 2616 | 2095 | 5629 |
| Panel baseline, repeat | 521 | 2616 | 2095 | 5627 |
| Mechanism only, defaults, first | 520 | 2616 | 2096 | 5627 |
| Mechanism only, defaults, repeat | 521 | 2615 | 2094 | 5631 |
| Final clock model, defaults | 652 | 2746 | 2094 | 5756 |
| Final clock model, flash-fetch=2 | 654 | 2780 | 2126 | 5794 |

The [baseline](cpu-evidence/baseline.log),
[default](cpu-evidence/verified-default.log), and
[weighted](cpu-evidence/verified-fetch2.log) logs are committed alongside the
repeat captures, with CRLF normalized to LF. Default instruction weights preserve the original timeline
within observed millisecond variability. Applying the configured 80 MHz
pre-app phase adds about 130–133 ms; after the firmware selects 160 MHz, the
SD-to-Home interval remains within 1 ms of baseline. The coordinator explicitly
accepted this pre-160 MHz shift in place of requiring an unchanged absolute
boot stamp. Flash-fetch=2 adds about 32 ms to the SD-to-Home interval; this is a
mechanism check, not the hardware calibration.

The firmware has no CPU-frequency log in these captures. QMP's
`human-monitor-command` with `xp /1wx 0x600c0008` and `xp /1wx 0x600c0058`
reads the configured clock at Home and after the low-power log. Both default
and weighted runs returned:

| State | CPU_PER_CONF | SYSCLK_CONF | Interpretation |
| --- | --- | --- | --- |
| Home | 0x00000001 | 0x000a8400 | PLL, period selection 1: 160 MHz |
| Low power | 0x00000001 | 0x000a8003 | XTAL 40 MHz / 4: 10 MHz |

See [QMP responses](cpu-evidence/verified-default.registers) and the
[weighted clock trace](cpu-evidence/verified-fetch2.trace), which records
80 MHz/factor 2 → 160 MHz/factor 1 → 10 MHz/factor 16. A separate stopped-CPU
qtest [register test](cpu-evidence/clock-register-test.log) and
[trace](cpu-evidence/clock-register-test.trace) also verify 40 MHz/factor 4.

`ninja -C build qemu-system-riscv32 qemu-system-xtensa` passes; only the existing
macOS duplicate `-liconv` linker warning remains. The instruction regression
test passes, including maximum costs, single-instruction TBs, MMIO reads, and
forced timer overruns. Physical-device calibration, migration/record-replay,
other RISC-V extensions, and cycle-counter accuracy remain unverified.
