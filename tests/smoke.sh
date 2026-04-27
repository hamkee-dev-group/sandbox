#!/bin/sh
set -eu

EXPECTED='Usage: ./sandbox <rootfs> [<target-binary>] [--user] [--userns] [--prepare-only] [--extras <file>] [--trace <args...>]'

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

assert_fails_with() {
	rootfs=$1
	shift
	expected=$1
	shift

	set +e
	actual=$(./sandbox "$rootfs" "$@" 2>&1)
	status=$?
	set -e

	if [ "$status" -eq 0 ]; then
		echo "smoke: expected failure for: ./sandbox $rootfs $*"
		exit 1
	fi
	if [ "$actual" != "$expected" ]; then
		echo "smoke: error output mismatch"
		echo "expected: $expected"
		echo "actual:   $actual"
		exit 1
	fi
}

tmp_root=$(mktemp -d)
trap 'rm -rf "$tmp_root"' EXIT HUP INT TERM

assert_fails_with "$tmp_root/no-target" "--prepare-only requires a target binary." --prepare-only
assert_fails_with --prepare-only "--prepare-only requires a target binary." "$tmp_root/leading-no-target"
assert_fails_with "$tmp_root/trace" "--prepare-only is not compatible with --trace." /bin/true --prepare-only --trace
assert_fails_with "$tmp_root/user" "--prepare-only is not compatible with --user." /bin/true --prepare-only --user
assert_fails_with "$tmp_root/userns" "--prepare-only is not compatible with --userns." /bin/true --prepare-only --userns

if [ "$(id -u)" -eq 0 ]; then
	prepare_root=$tmp_root/prepare
	./sandbox "$prepare_root" /bin/false --prepare-only

	if [ ! -x "$prepare_root/usr/bin/false" ]; then
		echo "smoke: prepare-only did not copy target to /usr/bin"
		exit 1
	fi
	if [ ! -x "$prepare_root/bin/false" ]; then
		echo "smoke: prepare-only did not preserve absolute target mirror"
		exit 1
	fi
	if [ ! -x "$prepare_root/bin/sh" ]; then
		echo "smoke: prepare-only did not copy /bin/sh"
		exit 1
	fi
else
	echo "smoke: skipping prepare-only rootfs assembly check (requires root)"
fi

echo "smoke: ok"
