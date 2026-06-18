# Board hardware notes: CanMV-K230 v1.1 vs BPI-CanMV-K230D-Zero

This document captures what was learned bringing the camera/ISP/RTSP pipeline up
on the **metalv** fork (Linux on the small core, CPU0, no RT-Smart). It compares
the two boards this tree builds for and explains *why* their camera bring-up
differs, so future work doesn't re-derive it from schematics.

Sources: `SCH_CanMV-K230_V1.1.pdf`, `SCH_BPI_CanMV-K230D-Zero_V1.1_2025-02-12.pdf`,
the `TYS-K230-200W-V2` camera-module datasheet (`banana_pi_bpi-d230d_zero_camera.jpg`),
the kernel DTs under `dl/linux/git/arch/riscv/boot/dts/canaan/`, and the vendor
sensor drivers in `k230_sdk/src/big/mpp/kernel/sensor/`.

> Both boards use the same Kendryte **K230** SoC (dual RV64: a "big" core with
> RVV + a "small" core without). The upstream dual-core SDK runs RT-Smart on one
> core and Linux on the other; **metalv runs Linux only, on the small core**, so
> anything RT-Smart used to do for the camera (master-clock setup, sensor reset)
> has to be done by Linux instead. See [`small_core_linux.md`](small_core_linux.md).

---

## 1. At a glance

| | CanMV-K230 v1.1 | BPI-CanMV-K230D-Zero |
|---|---|---|
| K230 package | K230 (separate LPDDR) | **K230D** (LPDDR4 in-package) |
| DRAM | **512 MiB** (`reg = <... 0x20000000>`) | **128 MiB** (`reg = <... 0x8000000>`) |
| Kernel DTS | `k230-canmv.dts` (model "Canaan CanMV-K230") | `bananapi-canmv-k230d-zero.dts` (model "Banana Pi CanMV K230D Zero") |
| defconfig | `k230d_canmv_defconfig`, `k230_canmv_*` | `BPI-CanMV-K230D-Zero_defconfig` |
| Display out | **HDMI** via LT9611 (DSI→HDMI bridge) + DSI | **DSI LCD only** (ST7701 480×800), no HDMI |
| Camera connector | FPC 0.5 mm 22-pin, **CSI1+CSI2** | FPC (AFC07) 24-pin **X1, CSI2** |
| Camera **MCLK** | **SoC-provided** (M_CLK2/M_CLK3) | **none** — module self-clocks (internal 24 MHz) |
| Camera I2C | I2C0 / I2C1 | **IIC4 → `/dev/i2c-0`** |
| Camera reset | dedicated `CAM0_RST` net | **`CAM0_GPIO` = GPIO62** (10 k pull-up) |

The board model strings differ: K230D reports `...K230D...`; the non-D boards
report `Canaan CanMV-K230`. This is the runtime signal used in `S99canaanboot`
to skip `vo_init` on K230D (see §4).

---

## 2. The GPIO62/63 dual-role — the crux of the camera difference

The single most important hardware difference is what the board does with K230
pins **GPIO62 / GPIO63** (BGA L13 / M13), which the SoC can mux as either GPIO,
`M_CLK2` / `M_CLK3` (camera master-clock outputs), or `UART3_DE/RE`.

* **CanMV-K230 v1.1:** GPIO62/63 are muxed to **M_CLK2/M_CLK3** and routed to the
  camera connector as `CAM_CLK0` / `CAM_CLK1`. The board therefore *supplies the
  sensor master clock*, and the documented module (OV5647) takes MCLK on its
  connector. Camera reset is a separate `CAM0_RST` net. Camera I2C is I2C0/I2C1.

* **BPI-CanMV-K230D-Zero:** GPIO62 is muxed to **plain GPIO** and used as the
  camera **reset** line (`CAM0_GPIO`, connector X1 pin 18, 10 k pull-up to 3V3 so
  released by default). GPIO63 is `CAM1_GPIO` for the second connector. There is
  **no MCLK pin on the CSI2 connector at all** — so **every module that works on
  this connector must supply its own oscillator**. This is a property of the
  *module*, not the sensor silicon:
    * The **gc2093** TYS-K230-200W-V2 module datasheet states it outright
      (`MCLK内置24MHZ`, "internal 24 MHz MCLK").
    * The **OV5647** silicon has *no* internal oscillator (it needs an external
      ~25 MHz XVCLK), yet the OV5647 module streams on X1 — therefore that module
      board necessarily provides its own XVCLK from an onboard oscillator.
  So on the K230D the *module* self-clocks; on the CanMV-K230 the *SoC* clocks the
  sensor (M_CLK2/M_CLK3; that board's OV5647 takes MCLK on connector pin 23).

Consequence for software: on the BPI K230D there is **nothing for the SoC to clock**.
Any MCLK-register manipulation is a *no-op* for that board's camera. The only
SoC-side action that matters is releasing the GPIO62 reset (and the pull-up
already does that). This is why MCLK fiddling never changed behavior there.

---

## 3. BPI-CanMV-K230D-Zero CSI2 camera connector (X1) — verified pin map

Connector X1 = AFC07 24-pin FPC, 1:1 with the TYS-K230-200W-V2 module pinout:

| X1 / module pin | Board net | K230 | Notes |
|---|---|---|---|
| 18 (RST) | `CAM0_GPIO` | **GPIO62** (BANK5, L13) | active-low reset, 10 k pull-up to 3V3 (R47) → released by default |
| 19 (PWDN) | — | **not connected** | module self-biases; not controllable from SoC |
| 20 (SCL) | `CAM0_SCL` = IIC4_SCL | GPIO7 | 4.7 k pull-up to 3V3 (R48) |
| 21 (SDA) | `CAM0_SDA` = IIC4_SDA | GPIO8 | 4.7 k pull-up to 3V3 (R49); bus = **`/dev/i2c-0`** |
| 22 (VDD3V3) | `VDD_3V3` | — | main 3V3 rail |
| 2/3,5/6,8/9 | CSI2 D4/D5/CLK ± | — | 2-lane MIPI CSI-2 |
| (no pin) | — | — | **no MCLK** — module has internal 24 MHz osc |

The kernel DTS retargets `&mipi0` to CSI2: `id = <2>`, `reg = <0x0 0x9000a800 0x0 0x800>`,
with CSI2 + M2 resets. Base `k230.dtsi` leaves the other MIPI instances disabled.

### Sensor modules observed on this board
* **OV5647** (I2C **0x36**): **works end-to-end** — ACKs on `/dev/i2c-0`, ISP opens,
  MIPI CSI-2 brings up (2 lanes, PHY 800M), V4L2 capture at 30 fps, RTSP streams.
  This is now the **default sensor** for `BPI-CanMV-K230D-Zero_defconfig`.
* **gc2093 / TYS-K230-200W-V2** (I2C **0x37**, the board's nominal shipping module):
  observed **silent on I2C** (empty `i2cdetect`, `gc2093: i2c write reg 03fe error
  121 Remote I/O`) *despite* correct GPIO62 reset, correct bus, internal MCLK and
  power. Because the **same slot/bus/reset shows 0x36 and streams with an OV5647
  swapped in**, this is a **physical** fault (FPC seating / orientation, or a bad
  module), **not** a software/board problem. No code change addresses a silent bus;
  re-seat the FPC (watch contact orientation) or try another gc2093 module.

Both sensor userspace drivers (`buildroot-overlay/package/vvcam/src/{ov5647,gc2093}.c`)
are pure I2C register-bangers on `/dev/i2c-0`; they do **no** GPIO/MCLK setup and
assume the sensor is already clocked and out of reset.

---

## 4. metalv small-core camera bring-up (what we added / fixed)

Because there is no RT-Smart, three things had to be handled in Linux/userspace.
All are sensor-independent and safe across both boards unless noted.

1. **Sensor bring-up before `isp_media_server`** —
   `buildroot-overlay/board/canaan/k230-soc/rootfs_overlay/etc/init.d/S99canaanboot`
   pulses GPIO62 reset and auto-detects the sensor on `/dev/i2c-0` (probes 0x37
   then 0x36). It also writes the SoC MCLK0 register; on the BPI K230D that write
   is a **harmless no-op** (the module self-clocks and the CSI2 connector has no
   MCLK pin), kept only because it is benign and may matter for other boards
   sharing this script. The script also **skips `vo_init` on K230D** (matched via
   `/proc/device-tree/model` containing `K230D`) to reclaim the display's
   in-kernel-pinned CMA for the camera/encoder; non-D boards keep `vo_init`.

2. **STREAMON `list_add` corruption fix** —
   `buildroot-overlay/package/vvcam/v4l2/isp/vvcam_isp_driver.c` flushes the pad
   buffer queue (`INIT_LIST_HEAD`) in the REQBUFS handler. Without it, stale freed
   vb2 buffers stay linked and the next STREAMON trips `CONFIG_LIST_HARDENED`.
   Generic V4L2/vb2 path — affects both sensors identically.

3. **CMA budget** — `linux-bpi.fragment` sets `CONFIG_CMA_SIZE_MBYTES=48`
   (was 32; matches `linux-canmv.fragment`) and `CONFIG_VPU_CANAAN=y` for the mvx
   H.264 encoder used by RTSP. 32 MiB could not hold ISP + display + mvx
   concurrently on the 128 MiB part (measured: only ~21 MiB free of 32 at idle,
   and a 720p capture needs a 1.35 MiB *contiguous* run the fragmented pool
   could not give — 640x480 fit, 720p did not until the bump).

### Sensor default selection
`BR2_PACKAGE_VVCAM_DEF_SENSOR` in each board defconfig selects the sensor compiled
into `vvcam_isp.ko` / driven by `isp_media_server`. The top-level `Makefile`
`SENSOR=` hook rewrites that line in `.config` and wipes `build/vvcam-*` to force a
recompile:

```sh
make CONF=BPI-CanMV-K230D-Zero_defconfig            # default: ov5647 (this board)
make CONF=BPI-CanMV-K230D-Zero_defconfig SENSOR=gc2093   # override for a gc2093 module
```

`BPI-CanMV-K230D-Zero_defconfig` now defaults to **ov5647** (the verified-working
module). The k230 / k230d_canmv board defconfigs are intentionally left on gc2093.

---

## 5. Streaming over the network

`camera_rtsp_demo` (built from `buildroot-overlay/package/camera_rtsp_demo`) serves
a live555 RTSP stream with hardware H.264 encode via the mvx codec (`/dev/video0`):

```sh
camera_rtsp_demo                       # H.264 1280x720 2000kbps from /dev/video1
camera_rtsp_demo -w 640 -h 480 -b 1000 # lighter, fits CMA most easily
# play:  ffplay -rtsp_transport tcp rtsp://<board-ip>:8554/test
```

On the 128 MiB BPI K230D, 640×480 streams comfortably at 48 MiB CMA. Higher
resolutions are bounded by CMA (total *and* fragmentation): if a large contiguous
allocation fails while `CmaFree` looks ample, reduce concurrent consumers (e.g.
the `vo_init` display) rather than only adding MiB.

---

## 6. Quick gotchas

* **GPIO62 means different things per board.** Camera *reset* on BPI K230D; camera
  *master clock* (M_CLK2) on CanMV-K230. Don't copy reset code across boards blindly.
* **BPI K230D camera modules self-clock.** Ignore MCLK when debugging a silent
  sensor there; check reset (GPIO62), I2C bus (`/dev/i2c-0` = IIC4), power, and —
  most often — the **FPC seating/orientation**.
* **A totally empty `i2cdetect` = physical.** A wrong driver/address still shows
  *something* at the real address. Nothing at all ⇒ bus not reaching the sensor.
* **Display CMA can't be reclaimed at runtime.** `killall vo_init` does not free it
  (framebuffers pinned in-kernel). Don't start it (K230D path) if you need the CMA.
* **`/dev/i2c-0` ≠ "I2C controller 0".** On BPI K230D it is the IIC4 controller
  (GPIO7/8). Trust the enumerated node, not the controller number.
