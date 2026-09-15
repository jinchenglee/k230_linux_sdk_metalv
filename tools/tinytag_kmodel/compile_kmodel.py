#!/usr/bin/env python3
"""Compile an external TinyTag ONNX model for the K230.

The source model and calibration images remain external to the repository.
"""

import argparse
import hashlib
import random
from pathlib import Path

import cv2
import nncase
import numpy as np
import onnx
from onnx import numpy_helper, shape_inference


INPUT_SHAPE = [1, 1, 360, 640]
OUTPUT_SHAPE = [1, 21, 45, 80]
IMAGE_SUFFIXES = {".bmp", ".jpeg", ".jpg", ".png", ".webp"}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--onnx", required=True, type=Path,
                        help="external TinyTag ONNX model")
    parser.add_argument("--calibration-dir", required=True, action="append", type=Path,
                        help="raw calibration-image directory; repeat as needed")
    parser.add_argument("--output", required=True, type=Path,
                        help="output .kmodel path")
    parser.add_argument("--samples", type=int, default=100,
                        help="number of calibration images (default: 100)")
    parser.add_argument("--seed", type=int, default=0)
    parser.add_argument("--quant-type", choices=("uint8", "int8"),
                        default="uint8", help="activation quantization")
    parser.add_argument("--weight-quant-type", choices=("uint8", "int8"),
                        default="uint8", help="weight quantization")
    parser.add_argument("--calibrate-method", choices=("Kld", "NoClip"), default="Kld")
    parser.add_argument("--finetune-weights", choices=("NoFineTuneWeights", "UseSquant"),
                        default="NoFineTuneWeights")
    parser.add_argument("--allow-small-calibration", action="store_true",
                        help="allow fewer than 100 images (experimental only)")
    parser.add_argument(
        "--keep-native-dilated-depthwise",
        dest="expand_dilated_depthwise",
        action="store_false",
        help="diagnostic opt-out: do not apply the default K230-safe dilation rewrite",
    )
    parser.set_defaults(expand_dilated_depthwise=True)
    parser.add_argument("--dump-dir", type=Path,
                        help="write nncase IR and quantization diagnostics here")
    return parser.parse_args()


def set_attribute(node: onnx.NodeProto, name: str, value: list[int]) -> None:
    replacement = onnx.helper.make_attribute(name, value)
    for index, attribute in enumerate(node.attribute):
        if attribute.name == name:
            node.attribute[index].CopyFrom(replacement)
            return
    node.attribute.append(replacement)


def expand_dilated_depthwise(model: onnx.ModelProto) -> int:
    """Rewrite depthwise dilation without changing the represented function."""
    initializers = {
        item.name: (index, item) for index, item in enumerate(model.graph.initializer)
    }
    expanded = 0
    for node in model.graph.node:
        if node.op_type != "Conv":
            continue
        attributes = {
            item.name: onnx.helper.get_attribute_value(item) for item in node.attribute
        }
        dilation = attributes.get("dilations", [1, 1])
        if dilation == [1, 1]:
            continue

        index, initializer = initializers[node.input[1]]
        weight = numpy_helper.to_array(initializer)
        group = attributes.get("group", 1)
        if (
            len(dilation) != 2
            or min(dilation) < 1
            or weight.ndim != 4
            or weight.shape[1] != 1
            or group != weight.shape[0]
        ):
            raise RuntimeError(
                "default dilation rewrite encountered an unsupported "
                f"non-depthwise convolution: {node.output[0]}"
            )

        height = (weight.shape[2] - 1) * dilation[0] + 1
        width = (weight.shape[3] - 1) * dilation[1] + 1
        dense = np.zeros(
            (weight.shape[0], weight.shape[1], height, width), dtype=weight.dtype
        )
        dense[:, :, ::dilation[0], ::dilation[1]] = weight
        model.graph.initializer[index].CopyFrom(
            numpy_helper.from_array(dense, initializer.name)
        )
        set_attribute(node, "dilations", [1, 1])
        set_attribute(node, "kernel_shape", [height, width])
        expanded += 1
        print(
            f"expanded {node.output[0]}: {tuple(weight.shape[2:])} "
            f"dilation {tuple(dilation)} -> {(height, width)}"
        )
    return expanded


def model_bytes(path: Path, expand_dilation: bool) -> bytes:
    model = onnx.load(str(path))
    if len(model.graph.input) != 1 or len(model.graph.output) != 1:
        raise RuntimeError("TinyTag conversion expects exactly one input and one output")

    if expand_dilation:
        count = expand_dilated_depthwise(model)
        print(f"expanded dilated depthwise convolutions: {count}")

    input_dims = model.graph.input[0].type.tensor_type.shape.dim
    if len(input_dims) != len(INPUT_SHAPE):
        raise RuntimeError(f"expected rank-4 input, got rank {len(input_dims)}")
    for dim, value in zip(input_dims, INPUT_SHAPE):
        dim.ClearField("dim_param")
        dim.dim_value = value

    model = shape_inference.infer_shapes(model)
    output_dims = model.graph.output[0].type.tensor_type.shape.dim
    actual_output = [dim.dim_value for dim in output_dims]
    if actual_output != OUTPUT_SHAPE:
        raise RuntimeError(f"expected output {OUTPUT_SHAPE}, got {actual_output}")
    onnx.checker.check_model(model)
    return model.SerializeToString()


def image_paths(directories: list[Path]) -> list[Path]:
    paths = sorted(
        path
        for directory in directories
        for path in directory.rglob("*")
        if path.is_file() and path.suffix.lower() in IMAGE_SUFFIXES
    )
    if not paths:
        raise RuntimeError("no calibration images found")
    return paths


def calibration_data(args: argparse.Namespace) -> list[np.ndarray]:
    paths = image_paths(args.calibration_dir)
    if len(paths) < args.samples:
        if not args.allow_small_calibration:
            raise RuntimeError(
                f"only {len(paths)} calibration images found, need {args.samples}; "
                "add raw images or pass --allow-small-calibration for an experimental build"
            )
        chosen = paths
    else:
        chosen = random.Random(args.seed).sample(paths, args.samples)

    images = []
    for path in chosen:
        gray = cv2.imread(str(path), cv2.IMREAD_GRAYSCALE)
        if gray is None:
            raise RuntimeError(f"cannot decode calibration image: {path}")
        if gray.shape != (INPUT_SHAPE[2], INPUT_SHAPE[3]):
            gray = cv2.resize(
                gray, (INPUT_SHAPE[3], INPUT_SHAPE[2]), interpolation=cv2.INTER_AREA
            )
        images.append(gray[None, None].astype(np.uint8))
    print(f"calibration images: {len(images)} of {len(paths)} available")
    return images


def main() -> None:
    args = parse_args()
    if args.samples <= 0:
        raise RuntimeError("--samples must be positive")
    if args.samples < 100 and not args.allow_small_calibration:
        raise RuntimeError("fewer than 100 samples requires --allow-small-calibration")
    if args.output.suffix != ".kmodel":
        raise RuntimeError("--output must end in .kmodel")

    images = calibration_data(args)
    options = nncase.CompileOptions()
    options.target = "k230"
    options.input_shape = INPUT_SHAPE
    options.input_type = "uint8"
    options.preprocess = True
    options.input_layout = "NCHW"
    options.input_range = [0.0, 255.0]
    options.mean = [0.0]
    options.std = [255.0]
    options.dump_ir = args.dump_dir is not None
    options.dump_asm = args.dump_dir is not None
    if args.dump_dir is not None:
        args.dump_dir.mkdir(parents=True, exist_ok=True)
        options.dump_dir = str(args.dump_dir)

    ptq = nncase.PTQTensorOptions()
    ptq.samples_count = len(images)
    ptq.quant_type = args.quant_type
    ptq.w_quant_type = args.weight_quant_type
    ptq.calibrate_method = args.calibrate_method
    ptq.finetune_weights_method = args.finetune_weights
    ptq.dump_quant_error = args.dump_dir is not None
    ptq.export_quant_scheme = args.dump_dir is not None
    ptq.export_weight_range_by_channel = args.dump_dir is not None
    ptq.set_tensor_data([images])

    compiler = nncase.Compiler(options)
    compiler.import_onnx(
        model_bytes(args.onnx, args.expand_dilated_depthwise), nncase.ImportOptions()
    )
    compiler.use_ptq(ptq)
    compiler.compile()

    data = compiler.gencode_tobytes()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(data)
    print(f"wrote: {args.output}")
    print(f"size: {len(data)} bytes")
    print(f"sha256: {hashlib.sha256(data).hexdigest()}")
    print(f"quantization: activations={args.quant_type}, weights={args.weight_quant_type}")


if __name__ == "__main__":
    main()
