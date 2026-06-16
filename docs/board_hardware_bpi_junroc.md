# BPI-CanMV-K230D-Zero / Junroc K230D hardware reference

Schematic-derived notes for the two 128 MiB K230D boards we support with a
single defconfig (`BPI-CanMV-K230D-Zero_defconfig`). Authoritative source:
[`SCH_BPI_CanMV-K230D-Zero_V1.1_2025-02-12.pdf`](../SCH_BPI_CanMV-K230D-Zero_V1.1_2025-02-12.pdf)
at the repo root. The Junroc K230D is "structurally identical to the
CanMV-K230D-Zero" per its product listing, and behaves the same in every
respect we have probed; the only known difference is the bundled camera
module (gc2093 vs ov5647).

## Building

```sh
# BPI-CanMV-K230D-Zero (default, gc2093 sensor on the CSI2 / X1 connector)
make CONF=BPI-CanMV-K230D-Zero_defconfig

# Junroc K230D (ov5647 sensor on the same connector)
make CONF=BPI-CanMV-K230D-Zero_defconfig SENSOR=ov5647
```

The `SENSOR=` Makefile hook rewrites `BR2_PACKAGE_VVCAM_DEF_SENSOR` in
`.config` and forces a vvcam rebuild. Only sensor names handled by the
ifdef chain in `buildroot-overlay/package/vvcam/v4l2/isp/vvcam_isp_driver.c`
(currently `ov5647`, `gc2093`, `gc2053`) are usable; other names will
build but boot with no default sensor.

## SoC and memory

- **SoC:** Kendryte K230D - dual-core XuanTie C908 RISC-V. CPU0 is RV64GC
  (no V extension); CPU1 is RV64GCV (has RVV). On this branch Linux runs
  only on CPU0. CPU1 is parked in OpenSBI HSM `STOPPED` and reserved for
  the bare-metal payload work described in
  [amp_bigcore_rvv_plan.md](amp_bigcore_rvv_plan.md).
- **DRAM:** 128 MiB LPDDR4 single die. DTS exposes `memory@0 reg = <0x0 0x0
  0x0 0x8000000>`. The upstream u-boot DTS variant splits the top 16 MiB
  off for a Nuttx co-processor partition (`0x7000000..0x8000000`); we
  currently give the full 128 MiB to Linux, but that 16 MiB block is the
  natural place to put the AMP reserved region in the future.
- **Storage:** microSD on K230 `MMC1` interface (`MMC1_CLK/CMD/D0..D3`).
- **CMA reservation:** `CONFIG_CMA_SIZE_MBYTES=32` in
  `buildroot-overlay/board/canaan/k230-soc/linux-bpi.fragment`. Rationale
  documented inline in that file.

## CSI camera connectors

Two physical connectors, mapped to different K230 MIPI controllers:

| Silkscreen | Schematic ref | Lanes | K230 controller `id` | I²C bus | Camera GPIO |
|---|---|---|---|---|---|
| **"CSI2"** (RPi-style 22-pin AFC07) | `X1` | `RX2` D4/D5 + CLK (2-lane) | `id=2` (CSI2), `reg=0x9000a800` | `i2c4` (CAM0_SDA/SCL) | `CAM0_GPIO` |
| **"CSI0+CSI1"** (31-pin FPC combo) | `J3` | `RX0` D0/D1 + CLK *and* `RX1` D2/D3 + CLK (2-lane each) | `id=0` (CSI0) and `id=1` (CSI1) | `i2c0` and `i2c1` | `CAM1_GPIO` |

The supported defconfig only wires the `X1` connector. The Linux DTS
`bananapi-canmv-k230d-zero.dts` programs `&mipi0 { id = <2>; reg = <0x0
0x9000a800 0x0 0x800>; ... }` — one MIPI controller, the high-throughput
CSI2 path. The combo `J3` connector is **deliberately left out of the
DTS** for simplicity (no current need for a second camera; enabling it
would mean adding `&mipi1 { id=1; ... }` and a separate `i2c0`/`i2c1` alias
wiring). If a future application needs two cameras, the IP, reset bindings,
and I²C buses are all already present in the SoC dtsi.

For both BPI-CanMV-K230D-Zero and Junroc K230D, the bundled sensor sits
on the `X1` connector:

- BPI ships **gc2093** (per the BPI-CanMV-K230D-Zero product config).
- Junroc ships **ov5647**.

Both are 2-lane MIPI CSI2 sensors compatible with the `id=2`
configuration.

## DSI display header

Schematic ref `J4`. 4-lane MIPI DSI plus touchscreen lines (TP_SDA/SCL/
INT/RST and LCD_RST/EN). The included Linux DTS pulls in
`display-st7701-480x800.dtsi` for an ST7701 480x800 panel, but the panel
itself is **not populated on either board out of the box**. Display
packages (`VO_INIT`, `LIBDRM_INSTALL_TESTS`, `VG_LITE`, `LVGL`, `FREETYPE`)
are intentionally not enabled in the defconfig for these boards - the
RTSP-over-network path doesn't need them, and 128 MiB of DRAM doesn't
have room to waste.

## USB and serial console

Two USB-C connectors with **completely different functions** - this is the
detail that explains the `ttyACM0` / `ttyACM1` mapping on the host:

| Connector | Schematic ref | Function |
|---|---|---|
| **`J1`** | `USB_TYPE_C` (left) | Board power input (5V) + debug UART(s) via the on-board `CH342K` USB-CDC bridge. Connect this to the host PC for console access; no driver setup needed in the K230 itself. |
| **`J2`** | `USB_TYPE_C` (right) | K230 `USB0` OTG direct - `USB0_DP/DN/ID`. This is the K230's own USB controller; use it for USB gadget mode (mass storage, CDC-ACM gadget, ADB) or as a USB host. |

The `CH342K` (`U7` on the schematic) is a WCH dual-channel USB-CDC-ACM
bridge IC. Its two channels are wired as:

- CH342K **channel 0** ↔ K230 `UART0_TXD/RXD` (IO38/IO39).
  K230's `console=ttyS0,115200` lands here, so host sees **Linux serial
  console on `/dev/ttyACM0`**.
- CH342K **channel 1** ↔ K230 `UART2_TXD/RXD` (IO5/IO6). A free UART for
  application use; host sees it as **`/dev/ttyACM1`**.

The CH342K presents itself to the host as a standard CDC-ACM device,
handled by the upstream `cdc_acm` kernel driver - no out-of-tree package
needed. No K230-side configuration is involved: the K230 just speaks
3.3V UART to the bridge, the bridge does all the USB enumeration.

This is why the Junroc K230D and BPI K230D Zero both expose a
`/dev/ttyACM*` console pair to the x86 host without any USB-gadget
plumbing in the Linux rootfs.

## WiFi

Schematic ref `U10`, silkscreen **`TL8821 / TL8189`**. The connection
pattern (`SDIO_DATA_0..3`, `SDIO_DATA_CMD`, `SDIO_DATA_CLK`, plus
`WL_REG_ON`, `WL_HOST_WAKE`, BT UART) matches a Realtek RTL8189FS or
RTL8821CS family SDIO Wi-Fi+BT combo module. SDIO goes to K230 `MMC0`
(`WIFI_CLK/CMD/D0..D3`).

**WiFi is currently not enabled in the defconfig.** The k230_canmv board
uses `BR2_PACKAGE_BCMDHD` (a Broadcom AP62xx driver), which is the wrong
chip family for these boards. Upstream's `5d18acd` (Rtl8377bs) and
`aad29a1` (rtl8733bs) commits don't cover the 8189/8821 family either.
Enabling Wi-Fi here is a follow-up: identify the exact silicon (read the
IC marking or capture a probe dmesg from a board running a Realtek
driver), then add the right out-of-tree driver package.

The rootfs overlay already has the userspace pieces wired:
`buildroot-overlay/board/canaan/k230-soc/rootfs_overlay/etc/network/interfaces`
reads `wlanssid`/`wlanpass` from u-boot env and brings up `wlan0` via
`wpa_supplicant`. Adding `BR2_PACKAGE_WPA_SUPPLICANT=y` + the right
driver to the defconfig is all that's needed once the driver is chosen.

## Boot mode and reset

Boot pins `BOOT0` (IO0) and `BOOT1` (IO1) select boot source via the
DIP/key switches near U12.3:

| BOOT1 | BOOT0 | Source |
|---|---|---|
| 0 | 0 | SPI Nor |
| 0 | 1 | SPI Nand |
| 1 | 0 | eMMC |
| 1 | 1 | SD card (the usual case for development) |

`SW1` is `RSTN` (system reset). `SW2` is `INT0` (boot select / user
button). `SW3` is the slide switch tied to the BOOT pins.

## ARGB LED

A WS2812B-Mini RGB LED (`D1`) is on `BANK2_GPIO35`. Useful for visible
liveness signaling from userspace once GPIO is exposed.

## Audio

Audio FPC connector `FPC1` (14-pin) exposes:

- `MIC_PL`, `MIC_NL`, `MIC_PR`, `MIC_NR` differential mic inputs
- `HP_OUTL`, `HP_OUTR` headphone outputs
- `MICBIAS`, three ADC channels (`ADC_0/1/2`)

Driven by the K230 internal codec; the DTS includes a `sound { compatible
= "canaan,k230-audio-inno"; ... }` node tied to the I²S controller +
inno-codec. Enabled by `BR2_PACKAGE_AUDIO_DEMO=y` and
`BR2_PACKAGE_AUDIO_REC_PLAY=y` in the defconfig.

## 40-pin GPIO header (JP1)

Standard 40-pin Raspberry-Pi-style header exposing I²C0, I²C2, UART1,
UART3, PWM0/1/2/3, SPI, multiple GPIOs, and 3.3V/5V/GND rails. Pin map is
in the schematic on page 1 of the PDF (the legend table to the right of
JP1).

## Quick capture recipes

Once the build flashes and boots, with the sensor plugged into the `X1`
(silkscreen "CSI2") connector and `isp_media_server` running:

```sh
# Confirm isp_media_server is up
ps | grep isp_media_server
cat /tmp/isp.err.log

# Headless NV12 capture
v4l2-ctl -d /dev/video1 \
  --set-fmt-video=width=1920,height=1080,pixelformat=NV12 \
  --stream-mmap=3 --stream-count=15 --stream-to=/tmp/csi.nv12

# Verify - first ~5 frames may be black during ISP AE warmup; check frame 14
dd if=/tmp/csi.nv12 bs=3110400 count=1 skip=14 2>/dev/null \
  | od -An -tu1 -w1 | sort -nu | head
```

For RTSP streaming once WiFi is up:

```sh
camera_rtsp_demo &       # sensible defaults, port 8554
# from host: ffplay rtsp://<board-ip>:8554/test.264
```

## Open verification items

These are things we should confirm once the board is flashed and boots:

1. **MIPI controller mapping on Junroc**: confirm the bundled ov5647 is
   physically on the `X1` (silkscreen "CSI2") connector, not on the
   combo `J3`. The user's testimony so far says yes.
2. **WiFi silicon variant**: read the IC marking on `U10` or capture a
   dmesg from a board running a Realtek SDIO driver. Determines whether
   the package is `rtl8189fs`, `rtl8821cs`, or something else.
3. **128 MiB headroom under load**: monitor `/proc/meminfo` and
   `/sys/kernel/debug/cma/cma-reserved` while running `camera_rtsp_demo`
   + isp_media_server + wpa_supplicant to confirm 32 MiB CMA is enough.
