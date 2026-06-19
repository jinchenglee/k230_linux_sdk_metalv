# K230 camera pipeline + sensor bring-up (color and mono / ISP-bypass)

How the K230/K230D camera path is wired in the **metalv small-core build**, what it
takes to add a **color** sensor (the supported path), and a concrete, file-level
plan for a **monochrome** sensor via the ISP's **raw passthrough** channel.

> Scope: metalv = Linux on CPU0, no RT-Smart. Camera stack = vvcam (kernel
> modules `vvcam_isp/mipi/vb/isp_subdev/video` + userspace `isp_media_server` +
> per-sensor userspace drivers). See also
> [`board_hardware_canmv_k230_and_bpi_k230d.md`](board_hardware_canmv_k230_and_bpi_k230d.md).

---

## 1. The pipeline (verified from the driver sources)

```
 sensor ──MIPI CSI-2──▶ DWC CSI-2 host (IPI) ──▶ VI / VICAP ──▶ ISP ──▶ MI ──▶ DDR ──▶ /dev/videoN (V4L2)
 (RAW8/10/12              k230_vi.c                k230_vi.c     vvcam_isp   memory          vvcam_video
  or YUV422)              DWC_IPI_DATA_TYPE        VI_ISP_CFG    subdev      interface
```

Key architectural facts established from the code:

- **The CSI front-end is a standard Synopsys DWC MIPI CSI-2 host + D-PHY.** It
  ingests `RAW8 (0x2A) / RAW10 (0x2B) / RAW12 (0x2C) / RAW16 (0x2E) / YUV422_8`
  (`k230_vi.h: enum csi_data_type`). Programmed in `k230_vi.c` via `DWC_IPI_*`.
- **The VI/VICAP block has NO DMA-to-DDR.** Its whole register map
  (`k230_vi.h`, `0x00–0xB0`) is PHY config, slave/HDR/flash-trigger, DVP-select
  and IRQ — there is **no frame-buffer-address / WDMA register**. The only path
  that writes pixels to DRAM is the **ISP's memory interface (MI)**.
  - **Consequence:** you cannot "remove the ISP" for capture — it owns the only
    DMA. "Bypass" means routing through the ISP's **raw passthrough channel**
    (no demosaic/AWB/CCM), not deleting the ISP block.
- **The ISP subdev exposes 5 pads per port** (`vvcam_isp_driver.h:75`):
  `SINK, SOURCE_MP, SOURCE_SP1, SOURCE_SP2, SOURCE_RAW` (`VVCAM_ISP_PORT_NR = 4`
  ports). In `vvcam_isp_set_fmt` (`vvcam_isp_driver.c:495`):
  - `SOURCE_MP/SP1/SP2` **force** their output to a processed YUV/RGB code
    (`mbus_fmt[0]`) → demosaiced pixels.
  - **`SOURCE_RAW` passes the sensor's format code straight through**
    (`source_pad->mbus_fmt[0].code = format->format.code`) → raw, unprocessed.
- **Video nodes are already linked to every pad** (`vvcam_pipeline_link.h`,
  `pipeline0[]`):

  | video_index | ISP remote_pad | Meaning | Today |
  |---|---|---|---|
  | 0 | 1 = `SOURCE_MP` | processed main (NV12) | this is `/dev/video1` |
  | 1 | 2 = `SOURCE_SP1` | processed self path | |
  | 2 | 3 = `SOURCE_SP2` | processed self path | |
  | **3** | **4 = `SOURCE_RAW`** | **raw passthrough** | **node exists, unused** |

- **`vvcam_video` already advertises RAW formats** (`vvcam_video_register.c:105–150`):
  `SRGGB8/SGRBG8/…`, `SBGGR10/SRGGB10/…`, `SBGGR12/…` (all **Bayer**), plus
  `NV12/NV16/YUYV/BGR24/P010`. **There is no `GREY`/`Y8`/`Y10` (mono) fourcc** and
  no `MEDIA_BUS_FMT_Y*` code anywhere yet.
- **Sensor drivers are pure userspace** (`package/vvcam/src/*.c`): they bang the
  sensor over `/dev/i2c-0` and do **no** GPIO/clock. Reset is now handled in the
  kernel (`vvcam_mipi` `reset-gpios`); modules on the K230D CSI2 connector
  self-clock. Present: `ov5647, gc2093, gc2053, imx335` — **all color Bayer**.

---

## 2. Color sensor (the supported path)

A color sensor flows `SINK → SOURCE_MP → /dev/video1` and the ISP does the full
Bayer pipeline (black-level → demosaic → AWB → CCM → gamma → NV12). Bring-up:

| Step | File(s) | Effort |
|---|---|---|
| Connector / adapter (if pinout differs) | — (mechanical) | — |
| DT: mipi `id`/lanes/`phy_freq`, `reset-gpios` | `buildroot-overlay/linux/00NN-*.patch` on the board dts | small (generic now) |
| Sensor driver: I²C init, mode tables, chip-ID | `package/vvcam/src/<sensor>.c` (model on `ov5647.c`) | small–moderate |
| **ISP tuning config** (calibration) | `package/vvcam/configs/<sensor>-WxH.{xml,json}` | **the tedious part** |
| Select default sensor | `configs/<board>_defconfig: BR2_PACKAGE_VVCAM_DEF_SENSOR` | trivial |

Estimate: a few days if a near-match tuning exists; ~1–2 weeks for good color
tuning from scratch. The hard part is *image quality*, not wiring.

---

## 3. Monochrome sensor — ISP-bypass via the RAW pad (the focus)

For a mono global-shutter sensor (OV9281, IMX296-LLR, …) the Bayer pipeline is
**wrong** (demosaic invents color from luma). The right path is the ISP **RAW
passthrough** channel: `SINK → SOURCE_RAW → video_index 3 node`, where the MI
writes the sensor's RAW8/RAW10 to DDR with **no color processing**. For a mono
sensor, **RAW8 *is* the finished grayscale image**.

### 3.1 Why this (and not a "true" ISP-less driver)

Because the **ISP MI is the only DMA-to-DDR** (§1), a truly ISP-less path would
mean re-implementing the MI from the TRM — large and redundant. The RAW pad gives
us raw-to-DDR while reusing the existing `vvcam_vb` buffer queue, the V4L2 node,
and CMA plumbing. So we keep the ISP *block* but skip its *processing*.

### 3.2 What already exists vs. what's missing

| Piece | State |
|---|---|
| CSI ingest of RAW8/10 | ✅ exists (`k230_vi.c` IPI data-type) |
| ISP `SOURCE_RAW` passthrough pad | ✅ exists (`vvcam_isp_driver.c:506`) |
| V4L2 node on the RAW pad (`video_index 3`) | ✅ wired (`vvcam_pipeline_link.h`) |
| RAW Bayer fourccs in `vvcam_video` | ✅ exists (`SRGGB8/10`…) |
| Mono `GREY/Y8/Y10` fourcc + `MEDIA_BUS_FMT_Y*` | ❌ missing |
| Mono sensor driver | ❌ missing (only color sensors) |
| ISP **raw-pipeline** daemon config | ❓ **unverified — main risk** |
| Reset / self-clock | ✅ generic (kernel `reset-gpios`; modules self-clock) |

### 3.3 Concrete plan (Path A — recommended: reuse the RAW pad)

1. **Add the mono sensor driver** — `package/vvcam/src/<sensor>.c`
   (model on `ov5647.c`): open `/dev/i2c-0`, chip-ID check, RAW8/RAW10 mode
   register tables, report sensor mbus format. Register it in the build/sensor
   selection the same way `ov5647`/`gc2093` are. *(Mono sensors are simpler —
   fewer registers, no color tuning.)*

2. **Provide a raw/passthrough ISP config** —
   `package/vvcam/configs/<sensor>-WxH.{xml,json}`. Start from an existing pair
   (e.g. `ov5647.{xml,json}`). The `SOURCE_RAW` channel is **confirmed working
   under the stock config** (§3.4), so the raw pad will emit frames; the
   sensor-specific work is making the config describe the mono sensor's geometry
   and feeding the ISP sink a non-Bayer RAW input. *(If the daemon ever refuses a
   mono/raw config, the Path B fallback below still applies.)*

3. **Add a real mono format (recommended for clean semantics)** — otherwise the
   frames come out tagged as Bayer `SRGGB8` and userspace must "know" they're
   luma. To do it properly:
   - `vvcam_video_register.c`: add a format-table entry
     `V4L2_PIX_FMT_GREY` (Y8) and/or `V4L2_PIX_FMT_Y10` ↔ `MEDIA_BUS_FMT_Y8_1X8`
     / `MEDIA_BUS_FMT_Y10_1X10`.
   - `vvcam_isp_driver.c`: add the `MEDIA_BUS_FMT_Y8_1X8`/`Y10_1X10` code to the
     `SOURCE_RAW` pad's accepted mbus list so `set_fmt` round-trips.
   - *(Skip this initially to prove the path; capture `SRGGB8` and treat bytes as
     Y. Add `GREY` once it streams.)*

4. **DT: point the mipi node at the connector + reset/enable** —
   new `buildroot-overlay/linux/00NN-*.patch`:
   - `&mipiX { id = <1|2>; reg/interrupts/resets for CSI1|CSI2; reset-gpios = <…>; }`
     (CSI2 reset = GPIO62 / `gpio1_ports 30`; CSI1 = GPIO63 / line 31).
   - If the module has a **power-enable** (e.g. IMX296 `CAM_IO0`), model it as a
     `regulator-fixed` with `gpio=…; enable-active-high; regulator-always-on;` —
     **not** as `reset-gpios` (it's hold-high, not a pulse).

5. **Select the sensor** — `BR2_PACKAGE_VVCAM_DEF_SENSOR="<sensor>"` (or `SENSOR=`).

6. **Userspace capture** — open the **RAW node** (`video_index 3`, enumerate the
   actual `/dev/videoN`), `VIDIOC_S_FMT` to `SRGGB8`/`GREY`, stream, and write/
   process frames as 8-bit grayscale (e.g. `ffplay -pix_fmt gray …` after dump).

**Risk / fallback (Path B — lean kernel driver, only if the daemon won't do raw):**
write a minimal kernel V4L2 driver that programs the DWC CSI IPI (`k230_vi.c`
already does this) **and the ISP MI raw channel directly** (frame-buffer address +
raw format), exposing a plain capture node, with **no `isp_media_server`**. This
needs the ISP MI register map from the K230 TRM (`docs/K230_Technical_Reference_
Manual_*.pdf`). More work, but removes the daemon and gives a deterministic mono
path. Recommended only if Path A's daemon config proves intractable.

### 3.4 Verification — CONFIRMED on hardware (BPI K230D + ov5647, 2026-06-18)

> **The raw-bypass path works end-to-end with the stock ISP config — no daemon
> reconfiguration was needed just to get raw frames.** This de-risks the main
> unknown in the mono plan: the ISP already feeds the `SOURCE_RAW` pad.

Verified node/format map on the running image:

| `/dev/video` | ISP pad | Formats advertised |
|---|---|---|
| `/dev/video1` | `SOURCE_MP` | NV12/NV16/YUYV/BGR3/BG3P/P010 (processed) |
| **`/dev/video4`** | **`SOURCE_RAW`** | **`GB10` (SGBRG10, 10-bit Bayer GBRG)** — passthrough |

(`video_index 3` → `SOURCE_RAW` enumerates as `/dev/video4`; `/dev/video0` is the
mvx VPU codec.)

**On the board (K230 / K230D) — find the node, then grab one raw frame:**
```sh
# 1. find the RAW node (the one advertising a Bayer fourcc, not NV12):
for d in /dev/video*; do echo "== $d =="; v4l2-ctl -d "$d" --list-formats 2>/dev/null \
    | grep -E "GB10|RG10|BG10|GR10|GREY|BA81|'.*'"; done
# 2. sensor mode sizes:
v4l2-ctl -d /dev/video4 --list-formats-ext
# 3. capture one raw frame to a file (match the active sensor W×H):
v4l2-ctl -d /dev/video4 \
  --set-fmt-video=width=1920,height=1080,pixelformat=GB10 \
  --stream-mmap --stream-skip=5 --stream-count=1 --stream-to=/tmp/raw.bin
ls -l /tmp/raw.bin     # expect width*height*2 bytes (≈4,147,200 @ 1920x1080)
```
Notes:
- `GB10` stores each pixel as **16-bit little-endian, 10 valid bits, LSB-aligned**
  → file = `W*H*2` bytes; the 8-bit image is `value >> 2`.
- **Exposure:** the raw frame uses whatever exposure the sensor has; in normal
  light the default is fine. (A dark/`max≈small` frame usually means *no light* —
  a lens cap will do it — or the sensor sitting at default with no AE; see §3.6.)

**On the host — diagnose and view (no Bayer codec needed in ffmpeg):**
```python
# raw_check.py — sanity + grayscale PNG from a GB10 frame
import numpy as np
from PIL import Image
W, H = 1920, 1080
raw = np.fromfile('raw.bin', dtype='<u2').reshape(H, W)   # 16-bit LE, 10-bit LSB
print('min/mean/max =', int(raw.min()), int(raw.mean()), int(raw.max()))
# interpret: max≈1000→full range (good); max≈tens→no light/low exposure;
#            max≈60000→data is MSB-aligned, use (raw >> 8) instead of (raw >> 2)
Image.fromarray((raw >> 2).astype('uint8')).save('raw.png')  # grayscale (Bayer mosaic for a color sensor)
```
Quick look without Python (any ffmpeg, treats 10-bit as 16-bit gray, ×64 to brighten):
```sh
ffplay -f rawvideo -pixel_format gray16le -video_size 1920x1080 -vf "lutyuv=y=val*64" raw.bin
```
For a **color** sensor (ov5647) the grayscale view shows a fine Bayer checkerboard
texture — expected. A **mono** sensor on this same path gives a clean grayscale
image (pure luma, no mosaic). To get a debayered *color* preview you'd need an
ffmpeg built with `bayer_*` pixel formats (`-pix_fmt bayer_gbrg10le`); not needed
to verify the path.

### 3.5b Verification ladder (for a new sensor)

1. **Sensor alive:** `i2cdetect -y 0` shows the sensor address after boot.
2. **Raw node present:** the RAW node advertises `SRGGBx`/`GREY` (sweep loop above).
3. **One raw frame:** the capture command above; check `min/mean/max` + the PNG.
4. **Continuous stream:** confirm fps and that CMA holds (mono RAW8 at 1280×800 ≈
   1 MB/frame — far lighter than NV12 color, so CMA pressure is low).

### 3.5 Effort summary (mono)

| Task | Size | Notes |
|---|---|---|
| Sensor driver | S | register tables from sensor vendor |
| Raw ISP config | S | ~~the risk~~ — **raw path confirmed streaming under stock config (§3.4)** |
| `GREY/Y8` format | S | optional but clean |
| DT mipi node + reset/enable | S | enable-gpio if power-enable module |
| Manual exposure/gain controls | S | replaces color AE on the bypass path (§3.6) |
| Capture app | S | v4l2 raw → grayscale |
| **(fallback) lean MI driver** | **L** | only if daemon can't do raw |

**Bottom line:** the mono raw path is *already plumbed and verified* (RAW pad +
raw video node + RAW fourcc → real frames on `/dev/video4`, §3.4). The original
"will the daemon feed the raw channel?" risk is **retired** — it does, out of the
box. What remains is purely additive: the sensor driver, an optional `GREY` format,
a DT node, and explicit exposure/gain. None of it belongs in `S99canaanboot`.

### 3.6 Exposure/gain on the raw path (design note)

The raw-bypass path skips the ISP's pixel processing **and its 3A/AE loop**. In
normal light the sensor's default exposure gives a usable frame (verified), but
there is **no auto-exposure** driving the sensor on a raw-only stream. For a
mono / machine-vision deployment this is usually *desirable* — you want
deterministic, fixed exposure. So the mono sensor driver should expose clean
**manual exposure (integration time) and analog/digital gain** controls over I²C
(standard V4L2 `V4L2_CID_EXPOSURE` / `V4L2_CID_ANALOGUE_GAIN`), rather than relying
on the color ISP's AE. If auto-exposure is genuinely needed, run a small userspace
control loop on the captured frames — far simpler than the full color 3A.

---

## 4. CSI1 vs CSI2

The three CSI receivers are identical standard DWC CSI-2 instances
(`kd_vi_bind_source(SOURCE_CSI0/1/2)`, `dwc_csi_phy_init(csi_num,…)`;
`k230.dtsi` has `mipi0/1/2`). Choosing CSI1 vs CSI2 is a **DT change** (retarget
the mipi node `id`/`reg`/`resets` and point `reset-gpios` at that connector's
line — CSI2 = GPIO62, CSI1 = GPIO63 on the BPI K230D). The capture/format work
above is identical regardless of which CSI receiver is used.
