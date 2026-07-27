# K230 AprilTag comparison demos

This package builds two applications around the same K230 camera, luma
preprocessing, display, keyboard-control, and FPS-reporting code:

- `apriltag_demo.elf`: the Rust `apriltag-rvv` detector
- `apriltag_c_demo.elf`: the official AprilRobotics C detector, version 3.4.5

The C detector is linked statically into its application. Its Buildroot source
archive and license are verified by `package/apriltag/apriltag.hash`.

## Build

```sh
make CONF=k230_canmv_defconfig apriltag_demo
```

The applications are installed in the target tree at:

```text
/root/app/apriltag_demo/
/root/app/apriltag_c_demo/
```

The package containing both is:

```text
output/k230_canmv_defconfig/images/deb/k230-apriltag-demo.deb
```

## Comparable runs

The C application's comparison defaults match the current Rust detector where
the algorithms expose equivalent controls: one detector thread, exact
Tag36h11 codewords (zero corrected bits), edge refinement off, decode
sharpening off, minimum blob size 25, and decimation factor 2.

```sh
cd /root/app/apriltag_demo
./apriltag_demo.elf --rvv --factor 2 --min-blob 25 \
    --csi-size 1280x720 --usb-video 3

cd /root/app/apriltag_c_demo
./apriltag_c_demo.elf --factor 2 --min-blob 25 \
    --csi-size 1280x720 --usb-video 3 --threads 1 \
    --bits-corrected 0 --decode-sharpening 0
```

Both applications start on CSI. Press `u` for the configured USB camera, `c`
for CSI, `n` to cycle the identical luma denoise modes, and `q` to quit.

Use `--upstream-defaults` with the C application to select the upstream
behavior: minimum blob size 5, two corrected bits, edge refinement on, and
decode sharpening 0.25. Use `--threads N` to measure the original detector's
multicore scaling independently of the RVV comparison.

The displayed `detect` rate includes only completed detector calls. Camera and
display rates are reported separately.
