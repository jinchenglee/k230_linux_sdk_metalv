# K230 AMP cache maintenance: physical versus virtual address operations

This note records a hardware-discovered requirement for sharing DRAM between
small-core Linux and a bare-metal program on the K230 big core. It applies to
the polling transport at physical address `0x1d000000` and should also guide
the later mailbox, RPMsg-Lite, and image-slot implementations.

## Operational conclusion

Use the K230 U-Boot physical-address range operations from big-core bare metal:

| Purpose | Instruction | Encoding with address in `a0` |
| --- | --- | --- |
| Clean a cache line by physical address | `dcache.cpa a0` | `0x0295000b` |
| Invalidate a cache line by physical address | `dcache.ipa a0` | `0x02a5000b` |
| Complete the operation | `sync.is` | `0x01b0000b` |

Align the start down to a 64-byte boundary, iterate to the end-exclusive
address in 64-byte increments, and execute `sync.is` after the range.

Do not use `dcache.cva`/`dcache.iva` for this bare-metal-to-Linux shared-memory
path. Those virtual-address operations are present in RT-Smart's C908 cache
implementation, but they did not make a larger dirty payload fully visible to
the other K230 core in our hardware test.

## Why the distinction matters

K230 U-Boot says that L2 is always enabled and implements its public physical
range APIs as follows:

- `flush_dcache_range()` loops over `dcache.cpa` and finishes with `sync.is`;
- `invalidate_dcache_range()` loops over `dcache.ipa` and finishes with
  `sync.is`; and
- whole-cache setup has separate D-cache and L2-cache operations.

The relevant source is:

```text
buildroot-overlay/boot/uboot/u-boot-2022.10-overlay/arch/riscv/cpu/k230/cache.c
```

RT-Smart's C908 port instead implements local virtual-address maintenance with
`dcache.cva`, `dcache.iva`, and `sync.s`:

```text
/mnt/sda_500gb/git_repo/k230_sdk/src/big/rt-smart/kernel/bsp/maix3/c908/cache.c
```

That RT-Smart code remains useful when an OS owns address translation and its
cache API defines virtual-address behavior. It is not the correct model for our
M-mode bare-metal endpoint operating on a protocol-defined physical carveout.
Even though the current bare-metal mapping is effectively identity-mapped, the
two instruction families are not interchangeable for visibility through the
K230 cache hierarchy.

## Hardware evidence

The initial AMP firmware used `dcache.cva`/`dcache.iva` with `sync.s`. The Linux
tester passed payloads through 4097 bytes, then failed on its first 65536-byte
exchange:

```text
PASS ... bytes=4097 ...
data mismatch: ... offset=0 expected=b3 actual=7d
```

`0x7d` was exactly byte zero of the immediately preceding 4097-byte response.
The response sequence and metadata were current, while the beginning of the
payload remained stale. This isolates the failure to payload cache visibility,
not publication sequencing or pattern generation. The larger request causes
dirty response lines to move beyond the small working set for which the
virtual-address operation appeared sufficient.

The firmware was consequently changed to the exact `dcache.cpa`/`dcache.ipa`
plus `sync.is` sequence used by K230 U-Boot. Hardware validation then completed
100 loops (900 exchanges) with zero CRC or byte mismatches. The important
boundary timings from that run were:

```text
4096 bytes:       0.230 ms
4097 bytes:       0.230 ms
65536 bytes:      3.675 ms
1048576 bytes:   58.669 ms
```

This closes the original stale-data failure for the polling implementation.

## Safe shared-memory sequence

Assume the cores are non-coherent. Keep every cache line single-writer and keep
payload, metadata, and publication words on separate cache lines.

For a big-core producer:

1. Write the payload.
2. Clean the payload with `dcache.cpa` for every covered cache line.
3. Complete physical cache maintenance with `sync.is`.
4. Write and clean producer-owned metadata.
5. Execute a release fence.
6. Write and clean the producer-owned publication sequence last.
7. Notify the peer by polling or mailbox.

For a big-core consumer:

1. Invalidate its producer-owned publication line with `dcache.ipa` and
   complete with `sync.is`.
2. If the sequence changed, execute an acquire fence.
3. Invalidate peer-owned metadata, inspect and validate its length.
4. Invalidate the exact peer-owned payload range.
5. Validate sequence and CRC before using the payload.

Never invalidate a line containing dirty data owned by the local core: an
invalidate can discard those writes. This is why control structures are padded
to 64 bytes rather than merely making individual fields atomic.

On the Linux side, the current diagnostic opens `/dev/mem` with `O_SYNC`. This
kernel maps that physical range uncached, so Linux does not issue matching
explicit cache instructions in the test. A future kernel driver must preserve
equivalent mapping and ordering semantics, for example through an appropriate
I/O or write-combined mapping, and must not silently replace it with ordinary
cacheable userspace memory.

## Follow-up validation

- Add tests with non-cache-line-aligned lengths and slot offsets.
- Record clean/invalidate cost separately from payload processing.
- Retain polling while adding mailbox interrupts so notification bugs cannot be
  mistaken for cache-coherency failures.
- Revalidate the same rules after introducing MMU mappings, DMA engines, or
  RPMsg-Lite vrings; transport ordering does not make external payload buffers
  coherent.
