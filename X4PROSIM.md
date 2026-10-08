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
x4prosim/run.sh flash.bin sd.img          # firmware log (USB-CDC) on stdout, SDL window if available
x4prosim/drive.py flash.bin sd.img log.txt wait:30 shot:home.png   # headless
```

`run.sh`/`drive.py` create a 1 GB MBR+FAT32 `sd.img` if it doesn't exist (`x4prosim/mksd.py sd.img 1024 books/` copies a folder in). Keep

### Firmware build gotchas (PlatformIO, pioarduino 6.1.19)

- **`ModuleNotFoundError: No module named 'SCons.Tool.FortranCommon'`** at the
  `firmware.elf` link step: the `tool-scons` package is half-installed. Delete it
  and rebuild; pio reinstalls it: `rm -rf ~/.platformio/packages/tool-scons`.
- **`*** Reinstall Arduino framework ***`** on the first build in a new checkout is
  normal (pioarduino keys its hybrid-compiled IDF on `sdkconfig.defaults`). It
  re-downloads `framework-arduinoespressif32` from GitHub releases and recompiles
  the IDF libs, so the first build takes several minutes.
- **`CERTIFICATE_VERIFY_FAILED` / `self-signed certificate in certificate chain`**
  during that download: pio's own Python uses its bundled certifi, which doesn't
  trust a corporate or sandbox proxy CA. Point it at the system bundle:
  `ln -sf /etc/ssl/certs/ca-certificates.crt "$(~/.platformio/penv/bin/python -c 'import certifi; print(certifi.where())')"`
  (or your proxy's CA file).
- **Never run two `pio run` at once**, even in different checkouts: they share
  `~/.platformio` and one will wipe the framework under the other.
- CI artifacts carry `firmware.bin` only. You need a local build for
  `firmware.elf` (symbols), `bootloader.bin` and `partitions.bin`.

`firmware.elf` beside you for symbols. To find a hang: run with `-s -S`, attach
`xtensa-esp32s3-elf-gdb firmware.elf` (`target remote :1234`), let it run, Ctrl-C,
`bt`. Or `info registers -a` on the HMP socket `/tmp/x4prosim-mon.sock` and
`xtensa-esp32s3-elf-addr2line -pfiaC -e firmware.elf <PC>`.

Unmapped peripheral registers read as 0 and ignore writes (catch-all
`esp32s3.iomem`), so a missing model usually shows up as a busy-wait on a status
bit, an `ESP_ERR_INVALID_STATE`, or a timeout in the log. Map new devices with
`memory_region_add_subregion_overlap(..., 1)` so they win over the catch-all.

## Where it stands (2026-10-08, 1007e firmware)

Machine `-machine x4pro` (in `hw/xtensa/esp32s3.c`: `x4pro_board_init`) boots the
unmodified `x4-pro-debug` image to the Home screen, takes key presses and renders
the panel.

Done on `x4prosim`:
- Octal PSRAM 8 MB (default on `x4pro`), plus a PSRAM bounds fix.
- `hw/misc/esp32s3_usb_jtag.c`: USB-CDC console (firmware log) to a chardev.
- `hw/misc/esp32s3_sens.c`: SAR ADC oneshot always "done" (IDF calibration at boot).
- `hw/gpio/esp32s3_gpio.c`: real GPIO model: OUT/ENABLE/IN, edge/level
  interrupts, named lines `pin-in` (drive a pin from outside), `pin-out` (pin level
  to a device), `wake` (light-sleep GPIO wakeup). Undriven inputs read 1 (pull-up).
- `hw/ssi/esp32s3_gpspi.c`: GPSPI2 CPU-mode master (W0..W15 buffer, USR, TRANS_DONE).
- `hw/display/uc8179.c`: the X4 Pro's UC8179 panel on GPSPI2 with CS/DC/RST and
  BUSY_N on GPIO, plus the bit-banged SDA reads the firmware uses for its panel
  probe and OTP. Answers like the real panel (VER, FLG, OTP), so the firmware logs
  `controller=UC8179 ... promoted=1`. Graphic console shown portrait like the
  device (`portrait=false` for raw). `hw/display/ssd1677.c` is an unused SSD1677
  model kept for reference.
- `-icount shift=2` (set by `run.sh`/`drive.py`): guest time follows instructions,
  not host speed; without it FreeRTOS can assert after light sleep on a slow host.
  Guest runs slower than real time, so give `wait:` steps generous values.
- `x4prosim/testdata/`: real device logs, the panel OTP dump, battery history to
  seed the SD card, measured power numbers. See its README.
- `hw/input/x4pro_keys.c`: Up/Down/Power keys (arrow keys + P, or
  `qom-set /machine/x4pro-keys down true`).
- `hw/misc/esp32s3_rtc_cntl.c`: light sleep: `SLEEP_EN` waits for the RTC timer
  alarm or a GPIO wake, and hides the sleep from the RTC counter so esp_timer
  isn't advanced twice.
- SD card: `x4prosim/mksd.py` makes an MBR + FAT32 image (SdFat needs the MBR);
  the existing `dwc_sdmmc` model mounts it.
- `x4prosim/drive.py`: headless runner with steps (`wait:S`, `press:down[:ms]`,
  `shot:file.png`, `hmp:cmd`). Example:
  `x4prosim/drive.py flash.bin sd.img log.txt wait:30 press:down shot:home.png`

Still missing: I2C (see the helper task below), LEDC frontlight, charger STAT
(GPIO21 reads 1 = charging), Wi-Fi, deep sleep with ext0/ext1 wake, USB OTG.

## Hardware to model (X4 Pro pin map, from freeink-sdk BoardConfig.h `XTEINK_X4_PRO`)

| Area | Hardware | Pins / bus | Owner |
| --- | --- | --- | --- |
| Display | UC8179 800x480 1-bit e-ink over GPSPI2, 10 MHz; SDA also read bit-banged | SCLK 12, SDA 11, CS 13, DC 18, RST 14, BUSY_N 6 | done |
| SD | SDMMC slot 1, 1-bit, 40 MHz; GPIO5 = power enable, active-LOW | CLK 41, CMD 42, D0 40 | done |
| Buttons | active-LOW, pull-up; Up 0 (strap), Down 7, Power 3 | GPIO | done |
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
   points at 0x8150, buffer-status clear on write 0x814E=0, INT pulse on GPIO10: give
   the GT911 a named GPIO out and connect it to the GPIO model's `pin-in` line 10 in
   `x4pro_board_init`. Its power enable is GPIO2 and reset GPIO4: take them from
   `pin-out` lines 2 and 4. Touch input: QEMU mouse events (`qemu_input_handler_register`
   with absolute pointer) mapped to the 480x800 portrait raw space, plus an HMP/QMP-
   friendly QOM property or `-device` option for scripted taps. The Home key: one
   key code in the GT911 key area (check the freeink-sdk GT911 driver for how it reads
   it). Respect power: no ACKs while GPIO2 is HIGH (powered off). Done when: a scripted tap moves
   the firmware's selection (the log prints activity/input lines).

Read the freeink-sdk drivers in the CrossDink checkout before modeling:
`freeink-sdk/libs/hardware/` (touch, gauge, RTC, BoardConfig). Model what the driver
actually uses; don't implement whole datasheets.

Verification for every PR: build clean (no new warnings), boot the 1007e-or-newer
`x4-pro-debug` image with `run.sh` for 60 s, paste the relevant log lines in the PR,
and confirm no new hang (`info registers -a` PCs keep moving).

## Don'ts

- Don't modify CrossDink or freeink-sdk. Don't change the GPIO, GPSPI, UC8179, keys or
  RTC_CNTL models (owned by the project thread); adding your devices' wiring lines to
  `x4pro_board_init` is fine. Ask in the PR if you need a hook elsewhere.
- Don't push to `x4prosim` directly. PRs only.
- No upstream references in PR titles, bodies or commits (no `#N` pointing at other
  repos, no `owner/repo#N`, no "fixes/closes" keywords).
