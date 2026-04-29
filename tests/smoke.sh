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

assert_fails_containing() {
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
	case $actual in
		*"$expected"*) ;;
		*)
			echo "smoke: error output mismatch"
			echo "expected to contain: $expected"
			echo "actual:              $actual"
			exit 1
			;;
	esac
}

tmp_root=$(mktemp -d)
trap 'rm -rf "$tmp_root"' EXIT HUP INT TERM

assert_fails_with "$tmp_root/no-target" "--prepare-only requires a target binary." --prepare-only
assert_fails_with --prepare-only "--prepare-only requires a target binary." "$tmp_root/leading-no-target"
assert_fails_with "$tmp_root/trace" "--prepare-only is not compatible with --trace." /bin/true --prepare-only --trace
assert_fails_with "$tmp_root/user" "--prepare-only is not compatible with --user." /bin/true --prepare-only --user
assert_fails_with "$tmp_root/userns" "--prepare-only is not compatible with --userns." /bin/true --prepare-only --userns

non_elf=$tmp_root/non-elf-target
printf '#!/bin/sh\nexit 0\n' > "$non_elf"
chmod +x "$non_elf"
assert_fails_with "$tmp_root/non-elf-root" "$non_elf is not a binary file" "$non_elf" --prepare-only

rootfs_file=$tmp_root/rootfs-file
printf 'not a directory\n' > "$rootfs_file"
assert_fails_containing "$rootfs_file" "rootfs path is not a directory: $rootfs_file" /bin/true --prepare-only

rootfs_parent_file=$tmp_root/rootfs-parent-file
printf 'not a directory\n' > "$rootfs_parent_file"
assert_fails_containing "$rootfs_parent_file/rootfs" "rootfs parent is not a directory: $rootfs_parent_file" /bin/true --prepare-only

if [ "$(id -u)" -ne 0 ]; then
	unwritable_parent=$tmp_root/unwritable-parent
	mkdir "$unwritable_parent"
	chmod 0555 "$unwritable_parent"
	assert_fails_containing "$unwritable_parent/rootfs" "rootfs parent is not writable/searchable: $unwritable_parent" /bin/true --prepare-only
	chmod 0755 "$unwritable_parent"
else
	echo "smoke: skipping unwritable rootfs path check (root can bypass directory permissions)"
fi

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
	extras_src=$tmp_root/extras_src
	extras_list=$extras_src/extras.txt

	mkdir -p "$extras_src/etc"
	printf 'payload bytes\n' > "$extras_src/payload.txt"
	printf 'app config\n' > "$extras_src/etc/app.conf"
	printf '%s\n' \
		"/bin/echo" \
		"payload.txt" \
		"etc/app.conf" \
		"var/run/iouringd/" \
		> "$extras_list"

	./sandbox "$extras_root" /bin/true --prepare-only --extras "$extras_list"
	if [ ! -x "$extras_root/bin/echo" ]; then
		echo "smoke: prepare-only extras did not copy absolute executable"
		exit 1
	fi
	if ! cmp -s "$extras_src/payload.txt" "$extras_root/payload.txt"; then
		echo "smoke: prepare-only extras did not copy relative file"
		exit 1
	fi
	if ! cmp -s "$extras_src/etc/app.conf" "$extras_root/etc/app.conf"; then
		echo "smoke: prepare-only extras did not copy nested relative file"
		exit 1
	fi
	if [ ! -d "$extras_root/var/run/iouringd" ]; then
		echo "smoke: prepare-only extras did not create relative directory"
		exit 1
	fi

	missing_list=$tmp_root/missing-extras.txt
	printf 'does-not-exist\n' > "$missing_list"
	set +e
	missing_output=$(./sandbox "$tmp_root/missing-extras-root" /bin/true --prepare-only --extras "$missing_list" 2>&1)
	status=$?
	set -e
	if [ "$status" -eq 0 ]; then
		echo "smoke: prepare-only extras missing source succeeded"
		exit 1
	fi
	case $missing_output in
		*"extras: failed to copy"*) ;;
		*)
			echo "smoke: prepare-only extras missing source message mismatch"
			echo "$missing_output"
			exit 1
			;;
	esac

	comment_root=$tmp_root/comment-extras-root
	comment_list=$tmp_root/comment-extras.txt
	printf '# comment\n\n/bin/echo\n' > "$comment_list"
	./sandbox "$comment_root" /bin/true --prepare-only --extras "$comment_list"
	if [ ! -x "$comment_root/bin/echo" ]; then
		echo "smoke: prepare-only extras did not copy valid entry after comment"
		exit 1
	fi
else
	echo "smoke: skipping prepare-only rootfs assembly check (requires root)"
fi

echo "smoke: ok"
