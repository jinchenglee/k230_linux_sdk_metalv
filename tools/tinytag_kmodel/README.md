# TinyTag K230 model toolchain

This directory turns an externally trained TinyTag ONNX model into a K230
`kmodel`, checks the compiled output against ONNX Runtime, and provides a
repeatable board comparison. Training checkpoints, ONNX models, TensorRT
engines, datasets, and generated work directories stay outside this repository.
Only a hardware-validated final `kmodel` belongs under
`buildroot-overlay/package/ai_demo/tinytag_detect/utils/`.

The scripts are fixed to the application contract:

- input: float ONNX tensor `1x1x360x640`, normalized to `[0, 1]`;
- deployed input: uint8 grayscale `1x1x360x640`, with `/255` preprocessing
  embedded by nncase;
- output: float tensor `1x21x45x80`;
- target/compiler: K230 and nncase 2.11.0.

## 1. Install the compiler environment

nncase 2.11 needs a .NET 7 runtime. Install .NET 7 using your operating
system's package instructions, then run:

```sh
tools/tinytag_kmodel/setup_venv.sh
```

The pinned Python environment is created at `tools/tinytag_kmodel/.venv` and
is ignored by Git. To keep it elsewhere, set `TINYTAG_NNCASE_VENV`. If .NET 7
is in a nonstandard location, set `TINYTAG_DOTNET_ROOT`; `run_nncase.sh` also
sets nncase's K230 plugin and simulator-helper paths.

## 2. Prepare calibration and validation images

Use raw, representative grayscale or color frames. They may be larger than
640x360; the compiler script converts to grayscale and resizes them exactly as
the deployed network input expects.

Use at least 100 calibration images covering normal, hard, and negative scenes.
Do not calibrate on the video or image set reserved for the final comparison.
Do not use images with rendered boxes/text unless no raw data exists: overlays
change activation statistics. Fewer than 100 images require the explicit
`--allow-small-calibration` flag and produce an experimental model.

## 3. Compile

The compiler intentionally supports only 8-bit activation and weight
quantization. The stock-compatible command is:

```sh
tools/tinytag_kmodel/run_nncase.sh \
  tools/tinytag_kmodel/compile_kmodel.py \
  --onnx /external/model/model.onnx \
  --calibration-dir /external/calibration/images \
  --output /tmp/model.int8.kmodel
```

For v40c, use the same command with its external ONNX path:

```sh
tools/tinytag_kmodel/run_nncase.sh \
  tools/tinytag_kmodel/compile_kmodel.py \
  --onnx "$HOME/Downloads/tinytag-v40c-unfrozen-moderate30ep/tinytag-v40c-unfrozen-moderate30ep.static.onnx" \
  --calibration-dir /external/raw/calibration/images \
  --output /tmp/tinytag-v40c-unfrozen-moderate30ep.int8.kmodel
```

### K230 dilated-depthwise workaround (enabled by default)

The current v40c ONNX contains `3x3` depthwise convolutions with dilation 2
and 3. nncase 2.11 lowers that native-dilation form incorrectly for the K230
INT8 path. This is the same toolchain bug encountered by the earlier model.
The deployed stock model avoids it by representing those operations as sparse
`5x5` and `7x7` kernels with dilation 1.

`compile_kmodel.py` now applies that rewrite automatically before every K230
compile. It inserts zeros between the original kernel taps, resets dilation to
1, prints every rewritten node and the total rewrite count, and keeps the
modified graph in memory. It does not create or retain another ONNX model.
Unsupported dilated non-depthwise convolutions fail explicitly instead of being
silently compiled through the suspect path.

The rewrite is mathematically exact: original and expanded FP32 graphs had zero
output difference across all 36 checked images. The unmodified dilated INT8
build had mean absolute error 0.35294 and incorrect heatmap maxima; the default
expanded INT8 build reduced error to 0.02799 and matched all 12 checked maxima.
The resulting 86,368-byte model uses the KPU-compatible path.

For toolchain investigation only, `--keep-native-dilated-depthwise` disables
the default rewrite. Do not deploy output built with that option unless a newer
nncase/runtime combination has passed both simulator and real-K230 validation.
Higher-precision quantization is intentionally not exposed: it leaves the
convolutions as floating-point CPU operations and defeats the KPU runtime goal.

## 4. Validate before copying to the board

Validation images should be held out from calibration:

```sh
tools/tinytag_kmodel/run_nncase.sh \
  tools/tinytag_kmodel/validate_kmodel.py \
  --onnx /external/model/model.onnx \
  --kmodel /tmp/model.kmodel \
  --images /external/held-out/images
```

The default gate requires mean absolute output error at most 0.05 and at least
90% of heatmap maxima within two output cells of FP32. Passing the simulator is
necessary but not sufficient: an earlier TinyTag kmodel passed simulation yet
corrupted memory on real K230 hardware.

## 5. Hardware safety test

Copy the candidate to `/tmp`, leaving the installed stock model untouched:

```sh
scp /tmp/model.kmodel root@BOARD:/tmp/
```

On the board, first run the no-camera operation profiler:

```sh
cd /root/app/tinytag_detect
./tinytag_detect.elf /tmp/model.kmodel ProfileOps 0.35 20 1.5 1
echo "exit=$?"
dmesg | tail -n 50
```

Then test a still image and a short live-camera run. Confirm that the process
exits cleanly and that unrelated processes still run normally before adding the
model to the root filesystem image.

## 6. Compare stock and candidate on one video

After both models are installed, copy `compare_on_board.sh` to the board and run:

```sh
sh compare_on_board.sh /tmp/evaluation.mp4 0.35 /tmp/tinytag-model-compare
```

Repeat at threshold `0.20`. The script uses identical `max_proposals=20` and
`roi_expand=1.5`, retaining per-frame profile logs and annotated videos for
both models. Copy the output directory back to the host for metric extraction
and side-by-side visualization.

## 7. Make a validated model part of the image

Copy only the compiled `kmodel` into
`buildroot-overlay/package/ai_demo/tinytag_detect/utils/`, add it to the CMake
install list, and add a separate launcher. Keep
`tinytag-v11_k230-v4c.int8.kmodel` and `run.sh` as the stock/default path until
the replacement decision is made. Build with:

```sh
make CONF=k230_canmv_small_core_defconfig tinytag_detect-dirclean
make CONF=k230_canmv_small_core_defconfig tinytag_detect
```

Finally repeat the on-device safety test with the files from
`output/k230_canmv_small_core_defconfig/target/root/app/tinytag_detect/`.
