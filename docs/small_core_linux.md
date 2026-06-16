# Linux on the K230 small core (no RVV)

Notes for the `opt_linux_on_small_core` branch. Corrects a couple of inaccuracies in earlier commit messages on this branch — see the "Corrigenda" section at the bottom.

## Goal

Run Linux on the K230's small core (CPU0), which lacks the RISC-V vector (RVV) extension, and obtain camera frames from one of:

- on-board MIPI CSI sensor (ov5647) via the vvcam ISP stack
- USB UVC camera (V4L2)
- network source (e.g. RTSP) — not implemented yet

On the running kernel the ISA reports as `rv64imafdc_zicbom_zicboz_zicntr_zicsr_zifencei_zihpm_zba_zbb_zbs_svpbmt` — no `v` flag, confirming the small core.

## Supported build configs

Only two defconfigs are actively maintained on this branch:

| defconfig | boards |
|---|---|
| `BPI-CanMV-K230D-Zero_defconfig` | Banana Pi K230D Zero, Junroc K230D |
| `k230_canmv_defconfig` | CanMV-K230-V1.1 |

Other defconfigs under `buildroot-overlay/configs/` are not maintained here.

## What is disabled, and why

The actual RVV-only vendor binaries are the AI/NN stack — these are turned off in both defconfigs:

- `BR2_PACKAGE_LIBNNCASE`
- `BR2_PACKAGE_AI2D_KPU`
- `BR2_PACKAGE_FACE_DETECT`
- all `ai_demo` packages

OpenCV4 and the V4L2 USB camera support are enabled and **built without RVV** so they execute on the small core.

CMA reservations were reduced from the vendor default (50% of DDR) to leave more DRAM for userspace:

- `buildroot-overlay/board/canaan/k230-soc/linux-bpi.fragment` → 16 MiB
- `buildroot-overlay/board/canaan/k230-soc/linux-canmv.fragment` → 48 MiB

A live 1080p preview through the ISP consumes ~5 MiB of CMA, so 48 MiB is comfortable.

## The camera stack is NOT RVV

This is the single most counter-intuitive point on this branch:

- The `vvcam` kernel modules (`vvcam_isp`, `vvcam_isp_subdev`, `vvcam_mipi`, `vvcam_vb`, `vvcam_video`) are plain RV64GC kernel code.
- `/usr/bin/isp_media_server` (the userspace ISP control daemon shipped by Canaan/VeriSilicon) is also RV64GC. `file isp_media_server` reports `RVC, double-float ABI` — no vector instructions. It is committed as a prebuilt blob at `buildroot-overlay/package/vvcam/isp_media_server` (added in commit `c1643f6`, 2024-05-22). The Debian variant `isp_media_server_debian` is the same program, statically linked.
- The supporting shared libraries (`libvvcam.so`, `libv4l2-drm.so`, `libdisplay.so`) are likewise RV64GC.

Empirically: on a board built from `k230_canmv_defconfig` running Linux on the small core, `test-display` produces a live ov5647 preview on HDMI, and `v4l2-ctl --stream-mmap` from `/dev/video1` writes real NV12 frames.

So **vvcam can stay enabled on small-core configs that need a CSI camera**. Only the AI/NN stack needs to stay off.

## V4L2 device map on `k230_canmv_defconfig`

| Node | Driver | Purpose |
|---|---|---|
| `/dev/video0` | `mvx` (Linlon v5276) | H.264/HEVC/VP8/VP9 m2m codec — **not a camera** |
| `/dev/video1..4` | `vvcam-video.0.0..3` | Four ISP output streams, fed by `vvcam-isp-subdev.0` on `/dev/media0` |
| `/dev/v4l-subdev0` | `vvcam-isp-subdev.0` | ISP control sub-device (the only V4L2 subdev — the ov5647 sensor is NOT a standard V4L2 entity; it is programmed via `vvcam-mipi` by `isp_media_server`) |
| `/dev/video5` | `uvcvideo` | USB UVC camera, YUYV (when plugged in) |
| `/dev/video6..8` | `uvcvideo` | UVC metadata / extension nodes |
| `/dev/media1` | `uvcvideo` | UVC media graph (`Camera 1` → `Processing 2` → `Extension 3` → video5) |

## Boot-time camera initialisation

`buildroot-overlay/board/canaan/k230-soc/rootfs_overlay/etc/init.d/S99canaanboot` runs at boot and does, in order:

1. `modprobe vvcam_isp vvcam_mipi vvcam_vb vvcam_isp_subdev vvcam_video`
2. `ISP_MEDIA_SENSOR_DRIVER=/usr/lib/libvvcam.so /usr/bin/isp_media_server >/dev/null 2>/tmp/isp.err.log &`
3. `vo_init &`

Sensor configs live under `/etc/vvcam/` (`ov5647.auto.json`, `ov5647.manual.json`, `ov5647.xml`, plus configs for bf3238, gc2053, gc2093). The default sensor is selected by `BR2_PACKAGE_VVCAM_DEF_SENSOR` (default `ov5647`).

## Capturing camera frames

### USB UVC — `/dev/video5`

```sh
v4l2-ctl -d /dev/video5 \
  --set-fmt-video=width=640,height=480,pixelformat=YUYV \
  --stream-mmap=3 --stream-count=30 --stream-to=/tmp/usb.yuv
```

### CSI / ISP — `/dev/video1`

Prerequisite: `isp_media_server` must be running (started automatically by `S99canaanboot`; check with `ps | grep isp_media_server`).

```sh
v4l2-ctl -d /dev/video1 \
  --set-fmt-video=width=1920,height=1080,pixelformat=NV12 \
  --stream-mmap=3 --stream-count=15 --stream-to=/tmp/csi.nv12
```

The first few frames after `STREAMON` are black while ISP AE/AWB warms up; drop the first ~5 in any capture utility.

To preview a captured NV12 file on a Linux host:

```sh
ffplay -f rawvideo -pixel_format nv12 -video_size 1920x1080 /tmp/csi.nv12
```

### CSI / ISP with on-device DRM display

`/usr/bin/test-display` (built from `buildroot-overlay/package/display/src/test.cpp`, ~35 lines) opens `/dev/video1` at 1920×1080 NV12, allocates a DRM/KMS plane via `libdisplay`, and links the two with dma-buf for zero-copy. No ISP setup — that is `isp_media_server`'s job.

`/usr/bin/v4l2-drm` (built from `buildroot-overlay/package/vvcam/v4l2-drm/`) is a more configurable variant that supports `-s` (no display, headless capture), crop, format selection, etc.

## Known caveats

- **k230d boards have no HDMI** on hand for this work; only the CanMV-K230-V1.1 has HDMI verified. Tests like `test-display` therefore only run on the CanMV board. On k230d, validate cameras via file dump or network.
- DRM logs `bpp/depth 32/24 not supported` for fbdev generic emulation. Cosmetic — DRM/KMS itself works (the HDMI preview proves it).
- `pgrep` is not in this image's busybox build. Use `ps | grep isp_media_server` instead.

## Corrigenda to earlier commit messages

- Commit `e1b49be` ("Focused on two build configs only…") says the change "remove[s] vvcam and its related kernel module … from `BPI-CanMV-K230D-Zero_defconfig`". The wording leaves the impression that vvcam is incompatible with running Linux on the small core. It is not: vvcam (both kernel modules and the `isp_media_server` userspace) is RV64GC and runs fine on the small core. `k230_canmv_defconfig` keeps vvcam, and it works. The removal in the BPI defconfig is a footprint/CMA choice (no on-board CSI sensor on the K230D Zero in the user's setup), not an RVV one.
- The same commit message claims the disabled NN modules "rely on binary library provided by vendor which contains RVV instructions". That is correct for libnncase / AI2D_KPU / face_detect / ai_demo — but should not be extrapolated to vvcam, which is RVV-free.
