#!/usr/bin/env python3
"""Gate a TinyTag kmodel against its FP32 ONNX source in nncase Simulator."""

import argparse
import math
from pathlib import Path

import cv2
import nncase
import numpy as np
import onnxruntime as ort


IMAGE_SUFFIXES = {".bmp", ".jpeg", ".jpg", ".png", ".webp"}
INPUT_HEIGHT = 360
INPUT_WIDTH = 640


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--onnx", required=True, type=Path)
    parser.add_argument("--kmodel", required=True, type=Path)
    parser.add_argument("--images", required=True, action="append", type=Path,
                        help="validation image or directory; repeat as needed")
    parser.add_argument("--samples", type=int, default=12)
    parser.add_argument("--max-mean-abs", type=float, default=0.05)
    parser.add_argument("--max-heat-peak-distance", type=float, default=2.0)
    parser.add_argument("--max-peak-fail-fraction", type=float, default=0.10)
    return parser.parse_args()


def collect_images(inputs: list[Path], samples: int) -> list[Path]:
    paths = []
    for item in inputs:
        if item.is_dir():
            paths.extend(
                path for path in item.rglob("*")
                if path.is_file() and path.suffix.lower() in IMAGE_SUFFIXES
            )
        elif item.is_file() and item.suffix.lower() in IMAGE_SUFFIXES:
            paths.append(item)
        else:
            raise RuntimeError(f"not an image or image directory: {item}")
    paths = sorted(set(paths))
    if not paths:
        raise RuntimeError("no validation images found")
    if samples <= 0:
        raise RuntimeError("--samples must be positive")
    if len(paths) <= samples:
        return paths
    indices = np.linspace(0, len(paths) - 1, samples, dtype=int)
    return [paths[index] for index in indices]


def main() -> None:
    args = parse_args()
    paths = collect_images(args.images, args.samples)
    session = ort.InferenceSession(str(args.onnx), providers=["CPUExecutionProvider"])
    input_name = session.get_inputs()[0].name
    simulator = nncase.Simulator()
    simulator.load_model(args.kmodel.read_bytes())

    mean_diffs = []
    peak_failures = 0
    for path in paths:
        gray = cv2.imread(str(path), cv2.IMREAD_GRAYSCALE)
        if gray is None:
            raise RuntimeError(f"cannot decode validation image: {path}")
        if gray.shape != (INPUT_HEIGHT, INPUT_WIDTH):
            gray = cv2.resize(gray, (INPUT_WIDTH, INPUT_HEIGHT), interpolation=cv2.INTER_AREA)
        uint8_input = gray[None, None].astype(np.uint8)
        fp32_input = uint8_input.astype(np.float32) / 255.0

        expected = session.run(None, {input_name: fp32_input})[0]
        simulator.set_input_tensor(0, nncase.RuntimeTensor.from_numpy(uint8_input))
        simulator.run()
        actual = simulator.get_output_tensor(0).to_numpy()
        if actual.shape != expected.shape:
            raise RuntimeError(f"output shape mismatch: {expected.shape} vs {actual.shape}")

        diff = np.abs(expected.astype(np.float32) - actual.astype(np.float32))
        mean_diffs.append(float(diff.mean()))
        expected_peak = np.unravel_index(np.argmax(expected[0, 0]), expected[0, 0].shape)
        actual_peak = np.unravel_index(np.argmax(actual[0, 0]), actual[0, 0].shape)
        peak_distance = math.dist(expected_peak, actual_peak)
        peak_failures += peak_distance > args.max_heat_peak_distance
        print(
            f"{path}: mean_abs={diff.mean():.6f} max_abs={diff.max():.6f} "
            f"heat_peak={expected_peak}/{actual_peak} distance={peak_distance:.2f}"
        )

    aggregate_mean = float(np.mean(mean_diffs))
    peak_fail_fraction = peak_failures / len(paths)
    print(
        f"summary: images={len(paths)} mean_abs={aggregate_mean:.6f} "
        f"peak_failures={peak_failures}/{len(paths)} ({peak_fail_fraction:.1%})"
    )
    failures = []
    if aggregate_mean > args.max_mean_abs:
        failures.append(
            f"mean abs error {aggregate_mean:.6f} exceeds {args.max_mean_abs:.6f}"
        )
    if peak_fail_fraction > args.max_peak_fail_fraction:
        failures.append(
            f"heatmap peak failure rate {peak_fail_fraction:.1%} exceeds "
            f"{args.max_peak_fail_fraction:.1%}"
        )
    if failures:
        raise SystemExit("validation FAILED: " + "; ".join(failures))
    print("validation PASSED")


if __name__ == "__main__":
    main()

