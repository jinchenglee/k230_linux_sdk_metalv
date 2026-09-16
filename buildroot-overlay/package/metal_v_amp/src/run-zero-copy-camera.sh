#!/bin/sh
set -eu

MODULE=${AMP_SHARED_POOL_MODULE:-/root/amp/k230_amp_camera_pool.ko}
TEST=${K230_ZERO_COPY_TEST:-/root/amp/rpmsg-zero-copy-camera}

if [ ! -c /dev/amp-shared-buffer-pool ]; then
	insmod "$MODULE"
fi

exec "$TEST" \
	--seconds "${K230_ZERO_COPY_SECONDS:-10}" \
	--timeout-ms "${K230_ZERO_COPY_TIMEOUT_MS:-5000}" \
	"$@"
