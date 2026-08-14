# AprilTag Sequence Streaming Inputs Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Keep at most one sequence row's encoded and decoded image data in memory while preserving secure identity checks and all-or-nothing output.

**Architecture:** Replace byte-owning sequence snapshots with hash-bearing `FileIdentity` metadata. Validate every input up front, securely reopen and verify one input in a lazy runner loader immediately before its calls, destroy that row's image before loading the next, then rehash all inputs and the manifest before emitting buffered telemetry.

**Tech Stack:** C++17, POSIX descriptor APIs, existing SHA-256 and CMake/CTest infrastructure, Buildroot cross-build/package flow.

---

### Task 1: Prove Lazy Runner Ordering

**Files:**
- Modify: `buildroot-overlay/package/apriltag_demo/bench/sequence_tests.cc`
- Modify: `buildroot-overlay/package/apriltag_demo/bench/sequence.h`
- Modify: `buildroot-overlay/package/apriltag_demo/bench/sequence.cc`

- [ ] **Step 1: Write the failing test**

Change runner tests to pass metadata rows and a loader callback. Record `load:first`, all first-row detections, `load:second`, all second-row detections, and final verification. Assert exactly that order so eager loading fails.

- [ ] **Step 2: Run test to verify it fails**

Run the host sequence test build and executable. Expect compilation to fail because `run_sequence` still accepts `std::vector<SequencePreparedInput>` without a loader.

- [ ] **Step 3: Write minimal implementation**

Define a metadata-only prepared row containing `SequenceRow` and identity string, define `LoadPreparedSequenceImage`, and make `run_sequence` call it once at the start of each loop iteration:

```cpp
for (const auto& input : inputs) {
    PreparedImage image = load_image(input);
    // Perform this row's cold and warm calls using image.
}
```

Keep records telemetry-only and preserve final verification before output.

- [ ] **Step 4: Run test to verify it passes**

Run the host sequence test executable. Expect the runner ordering and existing orchestration tests to pass.

### Task 2: Remove Retained File Bytes

**Files:**
- Modify: `buildroot-overlay/package/apriltag_demo/bench/sequence_tests.cc`
- Modify: `buildroot-overlay/package/apriltag_demo/bench/sequence.h`
- Modify: `buildroot-overlay/package/apriltag_demo/bench/sequence.cc`

- [ ] **Step 1: Write the failing tests**

Update file validation tests to require `FileIdentity` metadata (`path`, canonical path, device, inode, SHA-256) with no byte member. Add a transient reread test that mutates a file after the upfront pass and asserts row loading rejects it before decode.

- [ ] **Step 2: Run test to verify it fails**

Run the host sequence test build. Expect compilation failure because `FileSnapshot`/`SequenceInput` still expose retained bytes and no verified transient read API exists.

- [ ] **Step 3: Write minimal implementation**

Split descriptor reading into transient bytes plus `FileIdentity`. Make `load_sequence_inputs` perform secure upfront open/read/hash validation and return identities only. Add a secure row read that verifies canonical path, device, inode, and hash against the expected identity before returning transient bytes. Make final verification reopen and hash all files.

- [ ] **Step 4: Run test to verify it passes**

Run the host sequence tests. Expect all identity, symlink, replacement, mutation, and repeated-input tests to pass.

### Task 3: Stream Production CLI Rows

**Files:**
- Modify: `buildroot-overlay/package/apriltag_demo/bench/sequence_tests.cc`
- Modify: `buildroot-overlay/package/apriltag_demo/bench/sequence_main.cc`

- [ ] **Step 1: Write the failing test**

Create a two-row CLI manifest and instrument the decode callback and detector mock to assert the second decode occurs only after all first-row detector calls. Mutate an input between upfront validation and row decode and assert the transient reread rejects it without output.

- [ ] **Step 2: Run test to verify it fails**

Run the host sequence tests. Expect order assertions to fail because `sequence_cli` builds the complete prepared vector before calling the runner.

- [ ] **Step 3: Write minimal implementation**

Read the manifest bytes only long enough to parse and hash it, retain its `FileIdentity`, and release its bytes. Build metadata-only input identities. Pass a runner loader that securely rereads one row, validates identity/hash, decodes native dimensions, and returns one `PreparedImage`. Keep one detector handle and final input/manifest revalidation.

- [ ] **Step 4: Run test to verify it passes**

Run the host sequence tests. Expect CLI ordering, dimension mismatch, handle cleanup, secure reread, and output buffering tests to pass.

### Task 4: Full Verification

**Files:**
- Verify: `buildroot-overlay/package/apriltag_demo/bench/sequence.h`
- Verify: `buildroot-overlay/package/apriltag_demo/bench/sequence.cc`
- Verify: `buildroot-overlay/package/apriltag_demo/bench/sequence_main.cc`
- Verify: `buildroot-overlay/package/apriltag_demo/bench/sequence_tests.cc`
- Verify: `buildroot-overlay/package/apriltag_demo/CMakeLists.txt`
- Verify: `buildroot-overlay/package/apriltag_demo/apriltag_demo.mk`

- [ ] **Step 1: Run host tests**

Configure/build a host-compatible sequence test target and run `apriltag_sequence_tests`/CTest. Expect all tests to pass.

- [ ] **Step 2: Run cross-build**

Run the configured SDK `apriltag_demo` package rebuild. Expect `k230_apriltag_sequence_bench` and benchmark verification to build successfully.

- [ ] **Step 3: Run package verification**

Run the repository's Rust/package verification scripts and inspect the generated package contents for the sequence binary. Expect all checks to pass.

- [ ] **Step 4: Inspect final diff**

Confirm no sequence metadata type retains encoded bytes, no vector of prepared images remains, final verification precedes output, and unrelated dirty worktree changes remain untouched.
