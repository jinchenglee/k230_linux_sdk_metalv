# Generic neural-network profiler

`nn_profile` is a model- and framework-independent Linux profiler for statically
compiled neural networks. The runner loads a shared-object model plugin at
runtime through the C ABI in `include/nn_profile_plugin.h`.

The public SDK contains only the runner, ABI, and a synthetic test plugin. Model
conversion, weights, generated code, and optimized kernels stay in the model
provider's own build environment.

## Build

```sh
make CONF=k230_canmv_defconfig nn_profile
```

The target root filesystem installs the runner, ABI header, and wrapper under
`/root/app/nn_profile/`.

## Integrate a model

Implement `nn_profile_get_model_v1()` in a shared library. Its descriptor
declares the model's inputs and outputs, static batch size, semantic work counts,
build identity, and inference callback. The runner does not depend on the model's
source framework, compiler, graph format, operator set, or kernel implementation.

The provider is responsible for:

- compiling and linking the model for the target;
- adapting its native inference entry point to `nn_profile_run_v1`;
- reporting accurate tensor metadata and MAC/FLOP counts;
- keeping model artifacts outside this public repository; and
- validating numerical results against trusted golden outputs.

`tests/mock_model_plugin.c` is a minimal ABI example. It is built only when
CMake testing is enabled and is not installed in the production image.

## Run

```sh
./run_profile.sh ./model.k230.so \
  --input ./input_0.bin \
  --golden ./output_0.golden.bin \
  --atol 1e-5 --rtol 1e-5
```

Repeat `--input [INDEX:]FILE` and `--golden [INDEX:]FILE` for multi-input or
multi-output models. The default experiment is warm-cache, CPU-pinned, ten
warmups, and 100 measured calls. Use `--cache-scrub-mib N` for a deliberately
cache-disturbed scenario. The final `RESULT` line is stable key-value output
intended for automated collection.

Batch is a property of a compiled plugin. A batch-size study should compile one
plugin for each static batch. Repeating a batch-1 call eight times measures a
different workload from one batch-8 inference.

## Integrating optimized kernels

Add optimized kernels as the application requires them, preserve each variant's
exact dtype, layout, shape, ISA, alignment, scratch, and cache constraints, then
validate and profile the variant on hardware. Promote a variant into a reusable
kernel catalog only after its contract and benefit are established.

Application-specific fused or fixed-shape kernels can remain model-local. A
reusable catalog may contain multiple constrained variants of the same operation;
the model build selects among them before producing the plugin.

## Interpreting results across microarchitectures

VLEN is architectural register capacity, not necessarily vector execution
width. A vector instruction may execute over fewer physical lanes and take
multiple cycles. Raw cycle count and a theoretical `VLEN * frequency` peak are
therefore not portable performance measures.

Record and compare three layers:

- Semantic work: batch, tensor shapes, MACs/FLOPs, and bytes implied by the
  model contract.
- Architectural work: cycles, retired instructions, IPC, cache references and
  misses, plus the emitted SEW/LMUL/vector instruction mix.
- Target context: CPU, kernel, VLEN, compiler flags, model build ID, cache
  scenario, input identity, and output checksum.

Within one target, use cycles/inference, cycles/sample, FLOPs/cycle, cache
misses/inference, and scaling over static batch variants. Across targets,
normalize achieved FLOPs/cycle and bytes/cycle against measured compute and
streaming-load ceilings for that target using the same datatype and LMUL. This
separates kernel efficiency from the target's physical execution width.
