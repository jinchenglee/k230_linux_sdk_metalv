#!/bin/bash
# Fail the build if an installed executable contains RISC-V vector (RVV)
# instructions when Linux is configured for the scalar core.
#
# Motivation: a small-core rootfs that links a vector archive does not fail at
# build time, it SIGILLs on the board after camera negotiation, which reads as a
# camera or ISP fault. See docs/notes/small-core-rvv-pollution.md.
#
# Usage:
#   audit_vector_free.sh [--report-only] OBJDUMP ELF [ELF...]
#
# --report-only prints the same findings but always exits 0. It exists for
# binaries with a known-outstanding vector dependency that would otherwise make
# the image unbuildable -- currently tinytag_detect, which links the
# distributed RVV libnncase (see rvv-free-nncase-v2.11.0.md section 13). Drop
# the flag once that is packaged; do not add it to silence a new regression.
set -euo pipefail

REPORT_ONLY=0
if [ "${1:-}" = --report-only ]; then REPORT_ONLY=1; shift; fi
OBJDUMP="${1:?usage: audit_vector_free.sh [--report-only] OBJDUMP ELF [ELF...]}"
shift
[ "$#" -gt 0 ] || { echo "audit_vector_free.sh: no executables given" >&2; exit 2; }

command -v "$OBJDUMP" >/dev/null 2>&1 || [ -x "$OBJDUMP" ] || {
    echo "audit_vector_free.sh: objdump not usable: $OBJDUMP" >&2; exit 2; }

status=0
for elf in "$@"; do
    [ -f "$elf" ] || { echo "audit_vector_free.sh: missing $elf" >&2; exit 2; }
    # objdump output is tab-separated: <addr>:\t<encoding>\t<mnemonic>\t<operands>.
    # Vector mnemonics all begin with 'v'; scalar ones never do.
    # objdump output is tab-separated: <addr>:\t<encoding>\t<mnemonic>\t<operands>.
    # Group by originating library so the apriltag-rvv and libnncase causes stay
    # distinguishable -- they trap identically on the board.
    report="$("$OBJDUMP" -d "$elf" | awk -F'\t' '
        /^[0-9a-f]+ <.*>:$/ {
            sym = $0; sub(/^[0-9a-f]+ </, "", sym); sub(/>:$/, "", sym); next
        }
        NF >= 3 && $3 ~ /^v[a-z0-9._]+$/ {
            total++
            # Rust legacy mangling (-C symbol-mangling-version=legacy) ends in
            # 17h<hash>E; that is a far more reliable marker for "came out of
            # the Rust staticlib, including its -Z build-std rust-std" than any
            # crate-name prefix list.
            if (sym ~ /apriltag_rvv/) origin = "apriltag-rvv"
            else if (sym ~ /17h[0-9a-f]+E$/) origin = "rust-std (via libapriltag_rvv)"
            else if (sym ~ /nncase|optimized|^loop[0-9]+cpy|softmax/) origin = "libnncase"
            else origin = "other"
            by[origin]++
            if (!(sym in seen)) { seen[sym] = origin }
            cnt[sym]++
        }
        END {
            printf "TOTAL %d\n", total + 0
            for (o in by) printf "  %8d  %s\n", by[o], o
            n = 0
            for (s in cnt) {
                if (n == 0) printf "  top symbols:\n"
                printf "  %8d  %s\n", cnt[s], s
                if (++n >= 8) break
            }
        }')"
    total="$(printf '%s\n' "$report" | awk '/^TOTAL /{print $2}')"
    name="$(basename "$elf")"
    if [ "$total" -gt 0 ] && [ "$REPORT_ONLY" -eq 1 ]; then
        echo "RVV audit (report-only): $name has $total vector instruction(s)"
        printf '%s\n' "$report" | sed -n '2,$p'
        echo "  known-outstanding; see docs/notes/small-core-rvv-pollution.md"
    elif [ "$total" -gt 0 ]; then
        echo "RVV audit FAIL: $name has $total vector instruction(s) on a scalar-core build" >&2
        printf '%s\n' "$report" | sed -n '2,$p' >&2
        echo "  see docs/notes/small-core-rvv-pollution.md" >&2
        status=1
    else
        echo "RVV audit: $name is vector-free"
    fi
done
exit "$status"
