#!/bin/sh
set -eu

EXPECTED='Usage: ./sandbox <rootfs> [<target-binary>] [--user] [--userns] [--extras <file>] [--trace <args...>]'

actual=$(./sandbox 2>&1 || true)
if [ "$actual" != "$EXPECTED" ]; then
	echo "smoke: usage output mismatch"
	echo "expected: $EXPECTED"
	echo "actual:   $actual"
	exit 1
fi

if [ ! -x ./sandbox ]; then
	echo "smoke: ./sandbox is not executable"
	exit 1
fi

echo "smoke: ok"
