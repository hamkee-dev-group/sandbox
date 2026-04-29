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
repo_dir=$(pwd)

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

	extras_root=$tmp_root/prepare-extras
	extras_cwd=$tmp_root/extras-cwd/nested
	absolute_extra=$tmp_root/host-absolute/file.txt
	absolute_dotted_extra=$tmp_root/host-absolute/..cache/file.txt
	abs_parent_dir=$tmp_root/host-parent/src
	extras_list=$tmp_root/extras.txt

	mkdir -p "$extras_cwd/rel" "$extras_cwd/conf/..data" "$extras_cwd/a" "$tmp_root/host-absolute/..cache" "$abs_parent_dir"
	printf 'relative\n' > "$extras_cwd/rel/file.txt"
	printf 'relative dotted\n' > "$extras_cwd/conf/..data/file.txt"
	printf 'absolute\n' > "$absolute_extra"
	printf 'absolute dotted\n' > "$absolute_dotted_extra"
	printf 'blocked parent\n' > "$tmp_root/extras-cwd/blocked-parent.txt"
	printf 'blocked relative\n' > "$extras_cwd/blocked-relative.txt"
	printf 'blocked absolute\n' > "$tmp_root/host-parent/blocked-absolute.txt"
	printf '%s\n' \
		"$absolute_extra" \
		"rel/file.txt" \
		"conf/..data/file.txt" \
		"$absolute_dotted_extra" \
		"../blocked-parent.txt" \
		"a/../blocked-relative.txt" \
		"$abs_parent_dir/../blocked-absolute.txt" \
		> "$extras_list"

	set +e
	prepare_output=$(cd "$extras_cwd" && "$repo_dir/sandbox" "$extras_root" /bin/false --prepare-only --extras "$extras_list" 2>&1)
	status=$?
	set -e
	if [ "$status" -ne 0 ]; then
		echo "smoke: prepare-only extras failed"
		echo "$prepare_output"
		exit 1
	fi
	if ! printf 'absolute\n' | cmp -s - "${extras_root}${absolute_extra}"; then
		echo "smoke: prepare-only did not copy absolute extra to deterministic path"
		exit 1
	fi
	if ! printf 'relative\n' | cmp -s - "$extras_root/rel/file.txt"; then
		echo "smoke: prepare-only did not copy relative extra to deterministic path"
		exit 1
	fi
	if ! printf 'relative dotted\n' | cmp -s - "$extras_root/conf/..data/file.txt"; then
		echo "smoke: prepare-only rejected valid relative dotted component"
		exit 1
	fi
	if ! printf 'absolute dotted\n' | cmp -s - "${extras_root}${absolute_dotted_extra}"; then
		echo "smoke: prepare-only rejected valid absolute dotted component"
		exit 1
	fi
	if [ -e "$tmp_root/blocked-parent.txt" ] || [ -e "$extras_root/blocked-relative.txt" ] || [ -e "${extras_root}$tmp_root/host-parent/blocked-absolute.txt" ]; then
		echo "smoke: prepare-only copied literal parent-reference extra"
		exit 1
	fi
else
	echo "smoke: skipping prepare-only rootfs assembly check (requires root)"
fi

echo "smoke: ok"
