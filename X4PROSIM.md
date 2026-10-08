# x4prosim: instructions for helper agents

x4prosim is a fork of Espressif's QEMU (`esp-develop`) that runs the **unmodified**
CrossDink firmware for the **Xteink X4 Pro** (ESP32-S3R8, 16 MB flash, 8 MB octal
PSRAM). The goal is to run the real `x4-pro-debug` binary and see the screen, the SD
card and inputs working, so firmware can be tested without the physical device.

Rule zero: **never change the firmware to make it boot in QEMU.** Model the hardware
instead. Firmware lives in `prokrypt/CrossDink` (read-only for you; don't open PRs
or push there).

## Repo, branches, workflow

- Repo: `prokrypt/x4prosim`. Integration branch: **`x4prosim`** (base: Espressif
  `esp-develop` febae18).
- Work on your own branch off `x4prosim` (name it `ext/<area>`, e.g. `ext/i2c`) and
  open a PR into `x4prosim`. Keep each PR to one peripheral.
- Only touch the files your task names plus `hw/xtensa/esp32s3.c` (machine wiring)
  and the matching `meson.build`/`Kconfig`. Ask before editing shared files
  someone else owns (table below).
- Style: QEMU C style, 4-space indent. One-line comment only where the why isn't
  obvious. Each new device: a short header comment naming the registers it models
  and what it fakes.
- Commit messages: `x4prosim: <what>`.

## Build and run

```sh
# deps (Debian/Ubuntu)
sudo apt install ninja-build libglib2.0-dev libpixman-1-dev libgcrypt20-dev \
  libslirp-dev libsdl2-dev dosfstools python3-pip && pip install esptool
mkdir build && cd build
../configure --target-list=xtensa-softmmu --enable-gcrypt --enable-slirp --enable-sdl \
  --disable-strip --disable-user --disable-capstone --disable-vnc --disable-gtk --disable-docs
ninja qemu-system-xtensa
```

Firmware image: build CrossDink env `x4-pro-debug` with PlatformIO (`pio run -e
x4-pro-debug` in a CrossDink checkout), then:

```sh
x4prosim/mkflash.sh <CrossDink>/.pio/build/x4-pro-debug flash.bin
x4prosim/run.sh flash.bin sd.img          # firmware log (USB-CDC) on stdout
```

`run.sh` creates a 1 GB FAT32 `sd.img` if it doesn't exist. Keep
`firmware.elf` beside you for symbols. To find a hang: run with `-s -S`, attach
`xtensa-esp32s3-elf-gdb firmware.elf` (`target remote :1234`), let it run, Ctrl-C,
`bt`. Or `info registers -a` on the HMP socket `/tmp/x4prosim-mon.sock` and
`xtensa-esp32s3-elf-addr2line -pfiaC -e firmware.elf <PC>`.

Unmapped peripheral registers read as 0 and ignore writes (catch-all
`esp32s3.iomem`), so a missing model usually shows up as a busy-wait on a status
bit, an `ESP_ERR_INVALID_STATE`, or a timeout in the log. Map new devices with
`memory_region_add_subregion_overlap(..., 1)` so they win over the catch-all.

## Where it stands (2026-10-08, 1007e firmware)

Done on `x4prosim`:
- Octal PSRAM 8 MB works (`-global ssi_psram.is_octal=true`, plus a bounds fix).
- USB Serial/JTAG CDC console: `hw/misc/esp32s3_usb_jtag.c` (log to a chardev,
  1 kHz SOF so Arduino HWCDC sees a host).
- RTC SENS/SAR ADC oneshot: `hw/misc/esp32s3_sens.c` (always "done", mid-scale
  sample) so IDF ADC self-calibration at startup doesn't spin.
- The firmware boots to its main loop and runs the activity manager and render task.
  Remaining errors in the log:
  - `[SD] SDMMC volume mount failed` (the QEMU `dwc_sdmmc` model exists, but the mount fails)
  - `i2cWriteReadNonStop ... ESP_ERR_INVALID_STATE` every 6 s (no S3 I2C model)
  - the panel "works" only because BUSY reads 0; there is no GPSPI2 or panel model, so
    nothing is drawn

## Hardware to model (X4 Pro pin map, from freeink-sdk BoardConfig.h `XTEINK_X4_PRO`)

| Area | Hardware | Pins / bus | Owner |
| --- | --- | --- | --- |
| Display | SSD1677 800x480 1-bit e-ink over GPSPI2, write-only, 10 MHz | SCLK 12, MOSI 11, CS 13, DC 18, RST 14, BUSY 6 | Claude (project thread) |
| SD | SDMMC slot 1, 1-bit, 40 MHz; GPIO5 = power enable, active-LOW | CLK 41, CMD 42, D0 40 | Claude |
| Buttons | active-LOW, pull-up; Up 0 (strap), Down 7, Power 3 | GPIO | Claude |
| **I2C bus** | **ESP32-S3 I2C0 controller, 400 kHz** | **SDA 39, SCL 38** | **helper agent** |
| Touch | **GT911** at 0x5D (alt 0x14), INT 10, RST 4, power-enable GPIO2 active-LOW; reports X 0..480, Y 0..800 (portrait, firmware swaps XY and flips Y); has a capacitive Home key | on I2C | **helper agent** |
| RTC | **BM8563** (PCF8563-compatible) at 0x51 | on I2C | **helper agent** |
| Fuel gauge | **CW2017** at 0x63 | on I2C | **helper agent** |
| Frontlight | LEDC PWM 25 kHz 10-bit, cool GPIO8 (ch4), warm GPIO9 (ch5) | LEDC | open |
| Charger | STAT GPIO21, active-HIGH = charging | GPIO | open |
| Wi-Fi | not emulated by Espressif QEMU | n/a | open (later) |
| Sleep | deep sleep + ext0/ext1 GPIO wake, RTC_NOINIT/RTC_DATA memory | RTC_CNTL | open (later) |

## Helper agent task: I2C bus + GT911 + BM8563 + CW2017

1. **ESP32-S3 I2C controller** `hw/i2c/esp32s3_i2c.c`: start from
   `hw/i2c/esp32_i2c.c` (ESP32 classic) and adapt it to the S3 register map (IDF
   `components/soc/esp32s3/register/soc/i2c_reg.h`, `hal/esp32s3/include/hal/i2c_ll.h`).
   Notable S3 changes: command registers at a new offset, the `CTR.CONF_UPGATE` bit
   that latches config, FIFO via `I2C_DATA_REG`, and different interrupt bits.
   Arduino 3.3.9 uses the IDF **i2c_master (ng) driver**, which is interrupt-driven.
   Wire both I2C0 and I2C1 into `hw/xtensa/esp32s3.c` with their interrupt-matrix
   sources. Done when: no more `ESP_ERR_INVALID_STATE` in the log and an I2C probe of
   a missing address NACKs cleanly.
2. **BM8563** `hw/rtc/bm8563.c` (PCF8563 register set, BCD time from the host clock,
   writes kept). Done when: the firmware's RTC read gives host time.
3. **CW2017** `hw/misc/cw2017.c`: chip ID, VCELL, SOC, and a profile/config
   register set that accepts the driver's init writes. Battery % and voltage as QOM
   properties (`-global`) and settable at runtime via `qom-set`. Read the
   freeink-sdk driver for the exact registers it touches (`rg -n "CW2017|Cw2017"` in
   the CrossDink `freeink-sdk/` submodule). Done when: the status bar shows the
   configured %.
4. **GT911** `hw/input/gt911.c`: product ID "911", config at 0x8047.., status 0x814E,
   points at 0x8150, buffer-status clear on write 0x814E=0, INT pulse on GPIO10 (via
   a qemu_irq into the S3 GPIO model; expose it as a named GPIO out and connect it in
   the machine). Touch input: QEMU mouse events (`qemu_input_handler_register`
   with absolute pointer) mapped to the 480x800 portrait raw space, plus an HMP/QMP-
   friendly QOM property or `-device` option for scripted taps. The Home key: one
   key code in the GT911 key area (check the freeink-sdk GT911 driver for how it reads
   it). Respect power: no ACKs while GPIO2 is HIGH (powered off) if the GPIO model lets
   you read the output level; otherwise always on. Done when: a scripted tap moves
   the firmware's selection (the log prints activity/input lines).

Read the freeink-sdk drivers in the CrossDink checkout before modeling:
`freeink-sdk/libs/hardware/` (touch, gauge, RTC, BoardConfig). Model what the driver
actually uses; don't implement whole datasheets.

Verification for every PR: build clean (no new warnings), boot the 1007e-or-newer
`x4-pro-debug` image with `run.sh` for 60 s, paste the relevant log lines in the PR,
and confirm no new hang (`info registers -a` PCs keep moving).

## Don'ts

- Don't modify CrossDink or freeink-sdk. Don't touch the display, SDMMC or GPIO-input
  files (owned by the project thread); ask in the PR if you need a hook there.
- Don't push to `x4prosim` directly. PRs only.
- No upstream references in PR titles, bodies or commits (no `#N` pointing at other
  repos, no `owner/repo#N`, no "fixes/closes" keywords).
