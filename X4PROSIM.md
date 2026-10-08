# x4prosim: instructions for helper agents

x4prosim is a fork of Espressif's QEMU (`esp-develop`) that simulates the **Xteink
X4 Pro** (ESP32-S3R8, 16 MB flash, 8 MB octal PSRAM) closely enough to run **any
unmodified X4 Pro firmware**: the stock firmware, CrossInk, CrossDink, or a bare
ESP-IDF/Arduino app. The goal is firmware development without the physical device:
the screen, SD card, keys, touch, clock and battery behave like the board.

Rule zero: **never change a firmware to make it boot in QEMU.** Model the hardware
instead, from the datasheets and the board, not from one firmware's driver: another
firmware may use the same chip differently (hardware SPI CS, DMA, other panel
commands). CrossDink (`prokrypt/CrossDink`, read-only for you) is the reference
firmware for testing because its drivers are readable and it logs a lot.

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

Firmware image: any 16 MB X4 Pro flash image. Three ways to get one:
- A dump of a real device: `esptool.py --chip esp32s3 read_flash 0 0x1000000 flash.bin`
  (works only if the device doesn't use flash encryption).
- A PlatformIO or ESP-IDF build: `x4prosim/mkflash.sh <build dir> flash.bin` merges
  `bootloader.bin`, `partitions.bin` and the app (`firmware.bin`, or the only other
  `.bin`) at 0x0/0x8000/0x10000.
- A single app `.bin` released as an OTA update: flash a bootloader and partition
  table first (from any build), then the app at 0x10000.

CrossDink example: `pio run -e x4-pro-debug` in a CrossDink checkout, then:

```sh
x4prosim/mkflash.sh <CrossDink>/.pio/build/x4-pro-debug flash.bin
x4prosim/run.sh flash.bin sd.img          # firmware log (USB-CDC) on stdout, SDL window if available
x4prosim/drive.py flash.bin sd.img log.txt wait:30 shot:home.png   # headless
```

`run.sh`/`drive.py` create a 1 GB MBR+FAT32 `sd.img` if it doesn't exist (`x4prosim/mksd.py sd.img 1024 books/` copies a folder in). Keep

### CrossDink build gotchas (PlatformIO, pioarduino 6.1.19)

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
- UC8179 ink and ghosting (helper agent; full notes, data and tests in
  `x4prosim/ghosting/README.md`): each refresh (0x12) is simulated frame by frame
  instead of showing the planes, and plays on screen one frame per `frame-us`
  of virtual time (full refreshes flash, fast ones paint in). BUSY lasts the
  frames' length: boot full 1500 ms (device 1493), 50-frame direct gray 1250 ms
  (device 1189), DU 24 frames 600 ms (device 662).
  - PSR REG picks the waveform per refresh: REG=1 runs the uploaded register LUTs
    (0x20 VCOM, 0x21 WW, 0x22 KW, 0x23 WK, 0x24 KK); REG=0 runs stand-ins for the
    OTP waveforms (bodies never dumped): fast inside PTIN/PTOUT, full otherwise.
    CDI N2OCP copies NEW to OLD after the refresh.
  - Per pixel: saturating particle motion, two-component remnant voltage,
    blooming (edge loss), drift toward gray, shown linear in L*. Defaults are
    fitted to device photos: a typed-then-deleted word leaves ~11% ghost (device
    ~11%) that fades x0.53 over 6 fast refreshes (device x0.51); menus and gray
    book pages come out clean.
  - Every parameter is `-global uc8179.<name>=N` and 0 turns that mechanism off:
    `swing-frames` 6, `rail-soft` 100, `otp-fast-frames` 10,
    `otp-hold-drive` 6, `remnant-fast`/`-ms` 30/1000, `remnant-slow`/`-ms`
    10/30000, `bloom` 35, `drift`/`drift-s` 50/1800 (uncalibrated), `frame-us`
    25000. `otp-full-frames` is 30 (not 0 = off). Units and what each was fitted
    to: the notes, 3.4.
  - `busy-ms` 0 derives BUSY from the frames; nonzero fixes it. `animate=false`
    applies all frames at the DRF (BUSY still lasts the frames), for fast
    scripted runs. To capture the animation with `drive.py` (a screendump costs
    ~0.3 s), slow it: `-global uc8179.frame-us=100000`. A DRF during playback
    finishes the previous refresh first; RST stops it where it is.
  - Ink state is 16 B/pixel (6 MB) and the frame loop runs on QEMU's main loop.
  - Measure: `test/*-steps.txt` replay a scenario with `drive.py`,
    `tools/pepmeasure.py` / `seqmeasure.py` / `hist.py` read the screenshots,
    `tools/photomeasure.py` the device photos.
- `hw/display/esp_rgb.c`: the RGB console is created at realize, so `x4pro`
  (which never realizes it) no longer opens a stray black 800x600 SDL window.
- `hw/misc/esp32s3_sens.c`: temperature sensor always ready, 25 C (Goodies >
  Battery & Stats hung a core on it). `run.sh` shows the host cursor in SDL.
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

- Wi-Fi: the MAC is emulated with a fake AP bridged to a QEMU NIC (port of the
  lcgamboa ESP32-C3 model: `hw/misc/esp32s3_wifi.c`, `esp32_wifi_ap.c`,
  `esp32_wlan_packet.c`; analog/FE in `esp32s3_ana.c`, `esp32_fe.c`). `run.sh`
  joins it to a network:
  - `X4BR=br0 x4prosim/run.sh ...` bridges to your LAN: the firmware does its own
    DHCP with your router and is reachable at that IP like a real device. Needs a
    host bridge `br0` holding your Ethernet port, `allow br0` in
    `/usr/local/etc/qemu/bridge.conf`, and root (the helper is `build/qemu-bridge-helper`).
    Wi-Fi host NICs can't be bridged; use Ethernet.
  - Default: QEMU NAT (`-nic user,model=esp32_wifi,hostfwd=tcp::8080-:80`), DHCP
    10.0.2.15, host port 8080 reaches the device's port 80 (`curl localhost:8080`).
  - `X4NET="-nic ..."` overrides both; `drive.py` takes the same `-nic` in `QEMU_EXTRA`.
  A scan finds `PICSimLabWifi` (open, ch 1, the one that connects), `Espressif` and
  `MasseyWifi`. With no `model=esp32_wifi` NIC the MAC is a stub and a scan finds 0.
  Station MAC: the NIC's `mac=` (default 52:54:00:12:34:56) is burned into efuse
  when no efuse file is given; give each sim on one LAN its own `mac=`.

Still missing: LEDC frontlight, charger STAT (GPIO21 reads 1 = charging), real Wi-Fi
(WPA, signal, other APs),
deep sleep with ext0/ext1 wake, USB OTG.

Gaps another firmware is likely to hit (CrossDink doesn't need them yet):
- GPSPI: DMA transfers (IDF `spi_master` uses DMA above 64 bytes), hardware CS,
  command/address/dummy phases, and half-duplex reads on SDA.
- GPIO matrix / IO_MUX routing: pins are wired directly, so a peripheral routed to
  a different pin than the X4 Pro's won't reach the device.
- UC8179: the 0x90 partial window, LUTBD (0x25), VDHR, VCOM_DC value and
  temperature (TSSET) aren't modeled; the OTP waveforms are stand-ins until the
  full 0xA2 OTP is dumped.
  Open calibration items: `x4prosim/ghosting/README.md` section 8.
- Flash encryption and secure boot.

## Hardware to model (X4 Pro pin map, from freeink-sdk BoardConfig.h `XTEINK_X4_PRO`)

| Area | Hardware | Pins / bus | Owner |
| --- | --- | --- | --- |
| Display | UC8179 800x480 1-bit e-ink over GPSPI2, 10 MHz; SDA also read bit-banged | SCLK 12, SDA 11, CS 13, DC 18, RST 14, BUSY_N 6 | done |
| SD | SDMMC slot 1, 1-bit, 40 MHz; GPIO5 = power enable, active-LOW | CLK 41, CMD 42, D0 40 | done |
| Buttons | active-LOW, pull-up; Up 0 (strap), Down 7, Power 3 | GPIO | done |
| **I2C bus** | **ESP32-S3 I2C0 controller, 400 kHz** | **SDA 39, SCL 38** | done |
| Touch | **GT911** at 0x5D (alt 0x14), INT 10, RST 4, power-enable GPIO2 active-LOW; reports X 0..480, Y 0..800 (portrait, firmware swaps XY and flips Y); has a capacitive Home key | on I2C | done |
| RTC | **BM8563** (PCF8563-compatible) at 0x51 | on I2C | done |
| Fuel gauge | **CW2017** at 0x63 | on I2C | done |
| Frontlight | LEDC PWM 25 kHz 10-bit, cool GPIO8 (ch4), warm GPIO9 (ch5) | LEDC | open |
| Charger | STAT GPIO21, active-HIGH = charging | GPIO | open |
| Wi-Fi | MAC + fake open AP bridged to a QEMU NIC: NAT (`-nic user`, hostfwd) or LAN (`X4BR=br0`) | n/a | done |
| Sleep | deep sleep + ext0/ext1 GPIO wake, RTC_NOINIT/RTC_DATA memory | RTC_CNTL | open (later) |

## Helper agent task: GPSPI2 DMA + hardware CS (current)

Goal: firmware that drives the panel with ESP-IDF `spi_master` or Arduino `SPI`
using DMA and the controller's own CS works like CrossDink's CPU-mode SPI does
today. Branch `ext/spi-dma` off `x4prosim`, PR into `x4prosim`. You own
`hw/ssi/esp32s3_gpspi.c` for this task (and its wiring in `x4pro_board_init`).

Starting point: branch **`wip/spi-dma`** (598cb63) has an untested draft. It
compiles, but nothing has exercised it: GDMA out/in through the `esp_gdma`
API (`esp_gdma_get_channel_periph(GDMA_SPI2)`, read/write channel), command,
address and dummy phases, MOSI-only/MISO-only/full-duplex, a `cs0` output
(CS0_DIS, CS_KEEP_ACTIVE), and the panel CS = GPIO13 AND SPI CS0. Review it
against the S3 TRM and IDF `hal/esp32s3/include/hal/spi_ll.h` and fix what's wrong.

1. **DMA**: `spi_master` uses GDMA above 64 bytes (`SPI_DMA_CONF` TX/RX enable,
   GDMA PERI_SEL = 0 for SPI2). Check `esp_gdma_get_channel_periph`: it also
   matches any started channel, so a second DMA user (AES/SHA) could be picked.
   GDMA here only reaches internal DRAM; PSRAM buffers (EDMA) are a known gap:
   either add PSRAM to the GDMA address space or log a guest error.
2. **Interrupts and status**: TRANS_DONE plus whatever `spi_master`'s ISR and
   polling path read (check `spi_ll_usr_is_done`, `SPI_DMA_INT_*`, CMD.UPDATE,
   CMD.USR clearing). Transactions that queue back-to-back must work.
3. **Hardware CS**: CS0 goes low per transaction unless disabled or kept
   active. The GPIO matrix isn't modeled; CS0 is wired to the panel as if routed
   to GPIO13.
4. **Test firmware**: a minimal ESP-IDF (or Arduino) app in
   `x4prosim/tests/spi-dma/` that inits the X4 Pro panel pins
   (SCLK 12, MOSI 11, CS 13 as hardware CS, DC 18, RST 14, BUSY_N 6), sends
   UC8179 init + a 48000-byte DTM2 plane with one DMA transaction, refreshes
   (0x12), and prints "done". Commit its source and a build script, not the
   binary. Done when: `drive.py` screenshot shows the test pattern, and
   CrossDink 1007e still boots to Home with UC8179 promoted and a key press
   moving the selection (no regressions).

## Done helper task (merged): I2C bus + GT911 + BM8563 + CW2017

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

- Don't modify any firmware (CrossDink, freeink-sdk or others). Don't change the GPIO, UC8179, keys or
  RTC_CNTL models (owned by the project thread; GPSPI is yours for the SPI DMA task); adding your devices' wiring lines to
  `x4pro_board_init` is fine. Ask in the PR if you need a hook elsewhere.
- Don't push to `x4prosim` directly. PRs only.
- No upstream references in PR titles, bodies or commits (no `#N` pointing at other
  repos, no `owner/repo#N`, no "fixes/closes" keywords).
