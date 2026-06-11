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

make_long_dir() {
	base=$1
	min_len=$2
	path=$base
	i=0

	mkdir -p "$path"
	while [ "${#path}" -lt "$min_len" ]; do
		i=$((i + 1))
		path=$path/segment$i
		mkdir "$path"
	done
	printf '%s\n' "$path"
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

truncated_elf=$tmp_root/truncated-elf-target
printf '\177ELF' > "$truncated_elf"
chmod +x "$truncated_elf"
assert_fails_with "$tmp_root/truncated-elf-root" "$truncated_elf is not a valid ELF executable" "$truncated_elf" --prepare-only

rootfs_file=$tmp_root/rootfs-file
printf 'not a directory\n' > "$rootfs_file"
assert_fails_containing "$rootfs_file" "rootfs '$rootfs_file': not a directory" /bin/true --prepare-only

rootfs_parent_file=$tmp_root/rootfs-parent-file
printf 'not a directory\n' > "$rootfs_parent_file"
assert_fails_containing "$rootfs_parent_file/rootfs" "rootfs '$rootfs_parent_file/rootfs': parent '$rootfs_parent_file' is not a directory" /bin/true --prepare-only

missing_parent=$tmp_root/missing-parent/rootfs
assert_fails_containing "$missing_parent" "rootfs '$missing_parent': parent '$tmp_root/missing-parent': No such file or directory" /bin/true --prepare-only

if [ "$(id -u)" -ne 0 ]; then
	unwritable_parent=$tmp_root/unwritable-parent
	mkdir "$unwritable_parent"
	chmod 0555 "$unwritable_parent"
	assert_fails_containing "$unwritable_parent/rootfs" "rootfs '$unwritable_parent/rootfs': parent '$unwritable_parent' is not writable/searchable: Permission denied" /bin/true --prepare-only
	chmod 0755 "$unwritable_parent"

	traversal_root=$tmp_root/userns-traversal-root
	traversal_src=$tmp_root/traversal-src
	traversal_list=$traversal_src/extras.txt
	traversal_outside=$tmp_root/outside.txt
	traversal_abs_dir=$tmp_root/absolute-traversal
	traversal_abs="$traversal_abs_dir/foo/../escape"

	mkdir -p "$traversal_src" "$traversal_abs_dir"
	printf 'sentinel\n' > "$traversal_outside"
	printf 'absolute\n' > "$traversal_abs_dir/escape"
	printf '%s\n' \
		'../outside.txt' \
		"$traversal_abs" \
		> "$traversal_list"

	set +e
	traversal_output=$(./sandbox "$traversal_root" /bin/true --userns --extras "$traversal_list" 2>&1)
	status=$?
	set -e
	if [ "$status" -eq 0 ]; then
		echo "smoke: userns extras path traversal succeeded"
		exit 1
	fi
	case $traversal_output in
		*"../outside.txt"*) ;;
		*)
			echo "smoke: userns extras traversal stderr missing relative entry"
			echo "$traversal_output"
			exit 1
			;;
	esac
	case $traversal_output in
		*"$traversal_abs"*) ;;
		*)
			echo "smoke: userns extras traversal stderr missing absolute entry"
			echo "$traversal_output"
			exit 1
			;;
	esac
	if ! grep -qxF sentinel "$traversal_outside"; then
		echo "smoke: userns extras traversal overwrote file outside rootfs"
		exit 1
	fi
else
	echo "smoke: skipping unwritable rootfs path check (root can bypass directory permissions)"
	echo "smoke: skipping userns extras traversal check (requires non-root)"
fi

long_target_dir=$(make_long_dir "$tmp_root/long-target" 128)
long_target=$long_target_dir/true
cp /bin/true "$long_target"
long_root_parent=$(make_long_dir "$tmp_root/long-root" 945)
long_root=$long_root_parent/rootfs
assert_fails_containing "$long_root" "path too long: $long_root$long_target" "$long_target" --userns

if [ "$(id -u)" -eq 0 ]; then
	sym_root=$tmp_root/sym-root
	sym_sentinel=$tmp_root/sym-host-sentinel
	mkdir -p "$sym_root/usr/bin"
	printf 'a\n' > "$sym_sentinel"
	ln -s "$sym_sentinel" "$sym_root/usr/bin/true"
	set +e
	sym_output=$(./sandbox "$sym_root" /bin/true --prepare-only 2>&1)
	status=$?
	set -e
	if [ "$status" -eq 0 ]; then
		echo "smoke: prepare-only followed rootfs target symlink"
		exit 1
	fi
	if ! grep -qxF a "$sym_sentinel"; then
		echo "smoke: prepare-only overwrote host sentinel through target symlink"
		echo "$sym_output"
		exit 1
	fi

	parent_sym_root=$tmp_root/parent-sym-root
	parent_sym_host=$tmp_root/parent-sym-host
	parent_sym_sentinel=$tmp_root/parent-sym-sentinel
	mkdir -p "$parent_sym_root" "$parent_sym_host"
	printf 'a\n' > "$parent_sym_sentinel"
	ln -s "$parent_sym_host" "$parent_sym_root/usr"
	set +e
	parent_sym_output=$(./sandbox "$parent_sym_root" /bin/true --prepare-only 2>&1)
	status=$?
	set -e
	if [ "$status" -eq 0 ]; then
		echo "smoke: prepare-only followed rootfs parent symlink"
		exit 1
	fi
	if [ -e "$parent_sym_host/bin/true" ]; then
		echo "smoke: prepare-only wrote through rootfs parent symlink"
		echo "$parent_sym_output"
		exit 1
	fi
	if ! grep -qxF a "$parent_sym_sentinel"; then
		echo "smoke: prepare-only changed parent symlink sentinel"
		echo "$parent_sym_output"
		exit 1
	fi

	prepare_root=$tmp_root/prepare
	prepare_output=$(./sandbox "$prepare_root" /bin/false --prepare-only)

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
	if ! printf '%s\n' "$prepare_output" | grep -F "copied /bin/false -> $prepare_root/usr/bin/false" >/dev/null; then
		echo "smoke: prepare-only stdout missing /usr/bin target copy"
		echo "$prepare_output"
		exit 1
	fi
	if ! printf '%s\n' "$prepare_output" | grep -F "copied /bin/false -> $prepare_root/bin/false" >/dev/null; then
		echo "smoke: prepare-only stdout missing absolute target mirror copy"
		echo "$prepare_output"
		exit 1
	fi
	if ! printf '%s\n' "$prepare_output" | grep -F "copied /bin/sh -> $prepare_root/bin/sh" >/dev/null; then
		echo "smoke: prepare-only stdout missing /bin/sh copy"
		echo "$prepare_output"
		exit 1
	fi
	if ! printf '%s\n' "$prepare_output" | grep -F "TARGET /usr/bin/false" >/dev/null; then
		echo "smoke: prepare-only stdout missing target record"
		echo "$prepare_output"
		exit 1
	fi

	bad_dev_root=$tmp_root/prepare-bad-dev
	mkdir -p "$bad_dev_root/dev"
	printf 'not a device\n' > "$bad_dev_root/dev/null"
	assert_fails_containing "$bad_dev_root" "$bad_dev_root/dev/null exists but is not character device 1:3" /bin/true --prepare-only

	fake_ldd_dir=$tmp_root/fake-ldd-bin
	fake_ldd_root=$tmp_root/fake-ldd-root
	fake_ldd_marker=$tmp_root/fake-ldd-ran
	mkdir -p "$fake_ldd_dir"
	printf '#!/bin/sh\ntouch "%s"\n' "$fake_ldd_marker" > "$fake_ldd_dir/ldd"
	chmod +x "$fake_ldd_dir/ldd"
	set +e
	fake_ldd_output=$(PATH="$fake_ldd_dir:$PATH" ./sandbox "$fake_ldd_root" /bin/true --prepare-only 2>&1)
	status=$?
	set -e
	if [ "$status" -ne 0 ]; then
		echo "smoke: prepare-only with fake ldd on PATH failed"
		echo "$fake_ldd_output"
		exit 1
	fi
	if [ -e "$fake_ldd_marker" ]; then
		echo "smoke: prepare-only executed fake ldd from PATH"
		echo "$fake_ldd_output"
		exit 1
	fi

	runproof_root=$tmp_root/runproof
	set +e
	runproof_output=$(./sandbox "$runproof_root" /bin/true --prepare-only 2>&1)
	status=$?
	set -e
	if [ "$status" -ne 0 ]; then
		echo "smoke: prepare-only /bin/true failed"
		echo "$runproof_output"
		exit 1
	fi
	if [ ! -x "$runproof_root/usr/bin/true" ]; then
		echo "smoke: prepare-only did not copy true target to /usr/bin"
		exit 1
	fi
	set +e
	runproof_chroot_output=$(chroot "$runproof_root" /usr/bin/true 2>&1)
	status=$?
	set -e
	if [ "$status" -ne 0 ]; then
		echo "smoke: chroot run of /usr/bin/true from prepared root failed"
		echo "$runproof_chroot_output"
		exit 1
	fi

	seccomp_src=$tmp_root/seccomp-socket.c
	seccomp_helper=$tmp_root/seccomp-socket
	seccomp_compile_log=$tmp_root/seccomp-socket.compile
	cat > "$seccomp_src" <<'EOF'
#include <sys/socket.h>
#include <unistd.h>

int main(void)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return 1;
	close(fd);
	return 0;
}
EOF
	seccomp_compiler=
	for cc_candidate in ${CC:-} cc gcc; do
		if [ -z "$cc_candidate" ] || ! command -v "$cc_candidate" >/dev/null 2>&1; then
			continue
		fi
		set +e
		"$cc_candidate" "$seccomp_src" -o "$seccomp_helper" 2>"$seccomp_compile_log"
		status=$?
		set -e
		if [ "$status" -eq 0 ]; then
			seccomp_compiler=$cc_candidate
			break
		fi
	done
	if [ -z "$seccomp_compiler" ]; then
		echo "smoke: failed to compile seccomp socket helper"
		cat "$seccomp_compile_log"
		exit 1
	fi
	seccomp_root=$tmp_root/seccomp-socket-root
	set +e
	seccomp_output=$(./sandbox "$seccomp_root" "$seccomp_helper" 2>&1)
	status=$?
	set -e
	if [ "$status" -eq 0 ]; then
		echo "smoke: seccomp socket helper succeeded"
		echo "$seccomp_output"
		exit 1
	fi
	case $seccomp_output in
		*"[sandbox killed by signal "*) ;;
		*)
			echo "smoke: seccomp socket helper was not killed by seccomp"
			echo "$seccomp_output"
			exit 1
			;;
	esac

	fd_sentinel=$tmp_root/fd-sentinel
	fd_root=$tmp_root/fd-root
	printf 'host-original' > "$fd_sentinel"
	set +e
	fd_output=$(
		exec 3<>"$fd_sentinel"
		./sandbox "$fd_root" /bin/sh -c "printf 'sandbox-write\n' >&3" 2>&1
	)
	status=$?
	set -e
	if ! printf 'host-original' | cmp -s "$fd_sentinel" -; then
		echo "smoke: inherited fd write changed host sentinel"
		echo "$fd_output"
		exit 1
	fi
	if [ "$status" -eq 0 ]; then
		echo "smoke: inherited fd write unexpectedly succeeded"
		echo "$fd_output"
		exit 1
	fi

	if [ -x /usr/bin/strace ]; then
		trace_replay_root=$tmp_root/trace-replay
		trace_host_only=$tmp_root/trace-host-only
		printf 'host only\n' > "$trace_host_only"
		set +e
		trace_replay_output=$(./sandbox "$trace_replay_root" /bin/cat --trace "$trace_host_only" 2>&1)
		status=$?
		set -e
		if [ "$status" -ne 1 ]; then
			echo "smoke: trace replay cat returned unexpected status $status"
			echo "$trace_replay_output"
			exit 1
		fi
		case $trace_replay_output in
			*"[sandbox --trace exited with 1]"*) ;;
			*)
				echo "smoke: trace replay did not run traced command"
				echo "$trace_replay_output"
				exit 1
				;;
		esac
		if [ -e "$trace_replay_root$trace_host_only" ]; then
			echo "smoke: trace replay copied failed strace path"
			exit 1
		fi

		trace_mutating_root=$tmp_root/trace-mutating
		trace_mutating_path=/tmp/sandbox-trace-mutating-$$
		rm -f "$trace_mutating_path"
		printf 'host original\n' > "$trace_mutating_path"
		set +e
		trace_mutating_output=$(./sandbox "$trace_mutating_root" /bin/sh --trace -c "printf 'sandbox made\n' > '$trace_mutating_path'" 2>&1)
		status=$?
		set -e
		rm -f "$trace_mutating_path"
		if [ "$status" -ne 0 ]; then
			echo "smoke: trace mutating write returned unexpected status $status"
			echo "$trace_mutating_output"
			exit 1
		fi
		if ! grep -qxF 'sandbox made' "$trace_mutating_root$trace_mutating_path"; then
			echo "smoke: trace replay overwrote sandbox-authored file"
			echo "$trace_mutating_output"
			exit 1
		fi
	else
		echo "smoke: skipping trace replay check (/usr/bin/strace missing)"
	fi

	exec_root=$tmp_root/prepare-exec
	exec_marker=prepare-only-target-executed
	set +e
	exec_output=$(./sandbox "$exec_root" /bin/echo --prepare-only "$exec_marker" 2>&1)
	status=$?
	set -e
	if [ "$status" -ne 0 ]; then
		echo "smoke: prepare-only /bin/echo failed"
		echo "$exec_output"
		exit 1
	fi
	case $exec_output in
		*"[sandbox exited"*)
			echo "smoke: prepare-only entered runtime"
			echo "$exec_output"
			exit 1
			;;
	esac
	case $exec_output in
		*"$exec_marker"*)
			echo "smoke: prepare-only executed target"
			echo "$exec_output"
			exit 1
			;;
	esac
	if [ ! -x "$exec_root/usr/bin/echo" ]; then
		echo "smoke: prepare-only did not copy echo target to /usr/bin"
		exit 1
	fi
	if [ "$(chroot "$exec_root" /usr/bin/echo "$exec_marker")" != "$exec_marker" ]; then
		echo "smoke: prepared root did not execute /usr/bin/echo"
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

	extras_output=$(./sandbox "$extras_root" /bin/true --prepare-only --extras "$extras_list")
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
	case $extras_output in
		*"copied /bin/echo -> $extras_root/bin/echo"*) ;;
		*)
			echo "smoke: prepare-only stdout missing absolute extra copy"
			echo "$extras_output"
			exit 1
			;;
	esac
	case $extras_output in
		*"copied $extras_src/payload.txt -> $extras_root/payload.txt"*) ;;
		*)
			echo "smoke: prepare-only stdout missing relative extra copy"
			echo "$extras_output"
			exit 1
			;;
	esac
	case $extras_output in
		*"extras: copied"*)
			echo "smoke: prepare-only stdout duplicated extra file copy"
			echo "$extras_output"
			exit 1
			;;
	esac

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
