#!/bin/sh
#
# RPMsg-Lite AMP regression suite for the K230 dual-OS bring-up.
#
#   rpmsg-regression.sh              full run (a few minutes)
#   rpmsg-regression.sh --quick      short run, for a fast smoke check
#   rpmsg-regression.sh --post-boot  run this as the FIRST rpmsg traffic after
#                                    a boot; covers the cold-start regression
#                                    where the first pass over the vring
#                                    descriptor table dropped messages.
#
# Exit status is 0 only if every check passes.

TEST=${RPMSG_TEST:-/root/amp/rpmsg-echo-test}
DEV=${RPMSG_DEV:-/dev/rpmsg0}
pass=0
fail=0
mode=full

for arg in "$@"; do
	case "$arg" in
		--quick) mode=quick ;;
		--post-boot) mode=postboot ;;
		*) echo "usage: $0 [--quick|--post-boot]"; exit 2 ;;
	esac
done

check()
{
	name="$1"
	shift
	if "$@" > /tmp/rpmsg-reg.out 2>&1; then
		echo "PASS  $name"
		pass=$((pass + 1))
	else
		echo "FAIL  $name"
		sed 's/^/        /' /tmp/rpmsg-reg.out
		fail=$((fail + 1))
	fi
}

# A raw echo run prints FAIL itself on any timeout or mismatch and exits non-zero.
echo_run() { "$TEST" "$@"; }

stat_of()
{
	"$TEST" --stats 2>/dev/null | awk -v k="$1" '$1 == k { print $2 }'
}

echo "== environment"
check "$DEV exists" test -c "$DEV"
check "firmware stats block published" sh -c "\"$TEST\" --stats | grep -q '^magic'"
check "virtio driver_ok" sh -c "test \"\$($TEST --stats | awk '\$1==\"driver_ok\"{print \$2}')\" = 1"
check "name service announced" sh -c "test \"\$($TEST --stats | awk '\$1==\"announced\"{print \$2}')\" = 1"
check "no kernel BUG/warning" sh -c "! dmesg | grep -qiE 'BUG: sleeping|call trace|kernel BUG'"

if [ "$mode" = postboot ]; then
	# Regression guard: the first pass over the 256-entry descriptor table used
	# to lose 192 of the first 256 messages. Must be clean on first traffic.
	echo "== cold start (first traffic after boot)"
	check "first 1000 ping-pong lossless" echo_run --loops 1000 --timeout-ms 300
	check "first 300 burst lossless" echo_run --burst --loops 300 --size 496 --timeout-ms 500
fi

echo "== correctness"
if [ "$mode" = quick ]; then
	loops=200
else
	loops=2000
fi
check "payload sweep 1..496 B" echo_run --sweep --loops "$loops" --timeout-ms 1000

echo "== queue full / back pressure"
for n in 64 1024 4096; do
	check "burst $n x 496 B, no reader drain" echo_run --burst --loops "$n" \
		--size 496 --timeout-ms 500
done

if [ "$mode" = full ]; then
	echo "== soak"
	check "50000 x 496 B" echo_run --loops 50000 --size 496 --timeout-ms 1000
	check "50000 x 1 B" echo_run --loops 50000 --size 1 --timeout-ms 1000
fi

echo "== firmware accounting"
# Every buffer Linux published must have been fetched, delivered and echoed.
avail=$(stat_of rvq_avail_idx)
consumed=$(stat_of rvq_consumed)
rxcb=$(stat_of rx_callbacks)
fetch=$(stat_of fetch_rx)
txf=$(stat_of tx_failed)
# rvq_avail_idx and rvq_consumed are 16-bit vring indices and wrap at 65536;
# fetch_rx is a free-running counter. Compare modulo the vring index width.
fetch_wrapped=$((fetch % 65536))
echo "        rvq_avail=$avail rvq_consumed=$consumed fetch_rx=$fetch (mod 65536 = $fetch_wrapped) rx_cb=$rxcb tx_failed=$txf"
check "ring fully drained (rvq_consumed == rvq_avail_idx)" test "$consumed" = "$avail"
check "no dropped fetches (fetch_rx == rvq_consumed mod 2^16)" test "$fetch_wrapped" = "$consumed"
check "every fetch delivered (rx_callbacks == fetch_rx)" test "$rxcb" = "$fetch"
check "no failed sends" test "$txf" = 0

echo
echo "passed=$pass failed=$fail"
test "$fail" -eq 0
