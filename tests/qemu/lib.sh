# host side of the qemu tests, sourced after tests/lib.sh
# the variables that select qemu, the kernel and busybox are described in tests/README.md

Q_DIR=$top_builddir/tests/qemu
q_n=0

q_require() {
	local arch
	arch=$(uname -m)
	case $arch in
	x86_64)
		Q_QEMU=${NINIT_TEST_QEMU:-qemu-system-x86_64}
		Q_CON=ttyS0
		;;
	*)
		t_skip_all "qemu tests support x86_64 guests only, not $arch"
		;;
	esac
	command -v "$Q_QEMU" >/dev/null 2>&1 || t_skip_all "$Q_QEMU is not installed"
	Q_KERNEL=${NINIT_TEST_KERNEL:-}
	[ -n "$Q_KERNEL" ] || t_skip_all "NINIT_TEST_KERNEL is not set, see tests/README.md"
	[ -r "$Q_KERNEL" ] || t_skip_all "NINIT_TEST_KERNEL $Q_KERNEL is not readable"
	Q_BUSYBOX=${NINIT_TEST_BUSYBOX:-$(command -v busybox)}
	[ -x "$Q_BUSYBOX" ] || t_skip_all "busybox is not installed"
	[ -x "$NINIT_SHELL" ] || t_skip_all "$NINIT_SHELL is not executable"
	# the asan runtime reads /proc before pid 1 can mount it, and gcc links it as a shared library
	grep -qa __asan_init "$NINIT" && t_skip_all "ninit is built with a sanitizer, it cannot run as pid 1"

	Q_ACCEL=${NINIT_TEST_ACCEL:-}
	if [ -z "$Q_ACCEL" ]; then
		if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
			Q_ACCEL=kvm
		else
			Q_ACCEL=tcg
		fi
	fi
	if [ "$Q_ACCEL" = kvm ]; then
		Q_CPU=host
		Q_SCALE=${NINIT_TEST_SCALE:-1}
	else
		Q_CPU=max
		Q_SCALE=${NINIT_TEST_SCALE:-6}
	fi
	# a quiet build keeps NOTE, DONE and WAIT lines and the welcome off the console
	Q_QUIET=
	grep -q '^#define NINIT_QUIET' "$top_builddir/config.h" 2>/dev/null && Q_QUIET=1
	mkdir -p "$Q_DIR"
	q_base || t_bail "cannot build the base initramfs"
	q_tcg_check || t_skip_all "under tcg the guest cannot execute this host's binaries, make /dev/kvm usable or build the host without -march=native"
}

# tcg emulates a cpu that may lack instructions used by -march=native binaries of the host
q_tcg_check() {
	[ "$Q_ACCEL" = tcg ] || return 0
	(
		flock -x 9
		[ "$Q_DIR/tcg.ok" -nt "$Q_DIR/base.cpio" ] && exit 0
		[ "$Q_DIR/tcg.bad" -nt "$Q_DIR/base.cpio" ] && exit 1
		r=$Q_DIR/smoke
		rm -rf "$r"
		mkdir -p "$r/root/etc/ninit.d"
		printf 'kill -USR2 1\n' >"$r/root/etc/ninit.d/off"
		"$NINITCTL" init -d "$r/root/etc/ninit.d" --no-check >/dev/null 2>&1 || exit 1
		(cd "$r/root" && find . -mindepth 1 | "$Q_BUSYBOX" cpio -o -H newc -R 0:0 >"$r/scen.cpio" 2>/dev/null)
		cat "$Q_DIR/base.cpio" "$r/scen.cpio" >"$r/initrd"
		timeout $((60 * Q_SCALE)) "$Q_QEMU" -nodefaults -no-user-config -display none -no-reboot \
			-accel tcg -cpu max -m 384 -kernel "$Q_KERNEL" -initrd "$r/initrd" \
			-append "console=$Q_CON rdinit=/sbin/ninit panic=-1 loglevel=1" \
			-serial "file:$r/console" >/dev/null 2>&1
		if grep -q 'reboot: Power down' "$r/console"; then
			touch "$Q_DIR/tcg.ok"
			exit 0
		fi
		touch "$Q_DIR/tcg.bad"
		exit 1
	) 9>"$Q_DIR/.tcg.lock"
}

# whether TEXT can reach the console of this build
q_visible() {
	[ -z "$Q_QUIET" ] && return 0
	case $1 in
	*"NOTE > "* | *"DONE > "* | *"WAIT > "* | *Welcome* | *"poweroff: now"* | *"reboot: now"* | \
		*"halt: now"* | *"shutdown: stopping"* | *"poweroff: s"* | *"boot: "*" of "*) return 1 ;;
	esac
	return 0
}

# the program interpreter, whose path the first page of a dynamic ELF file holds
q_interp() {
	head -c 4096 "$1" | tr '\0' '\n' | grep -a -m1 '^/.*/ld-.*\.so'
}

# copies FILE and the shared objects it needs into ROOT, at the same paths
q_copy_elf() {
	local root=$1 f=$2 dst=${3:-$2} lib libs interp
	mkdir -p "$root${dst%/*}"
	cp -L "$f" "$root$dst" || return 1
	# the ldd of glibc cannot list a musl program, the musl loader can
	interp=$(q_interp "$f")
	case $interp in
	*/ld-musl-*) libs=$("$interp" --list "$f" 2>/dev/null) ;;
	*) libs=$(ldd "$f" 2>/dev/null) ;;
	esac
	for lib in $(grep -o '/[^ )]*' <<<"$libs"); do
		[ -e "$root$lib" ] && continue
		mkdir -p "$root${lib%/*}"
		cp -L "$lib" "$root$lib" || return 1
	done
}

# the base initramfs holds the programs under test, bash, busybox and the guest library
q_base() {
	local base=$Q_DIR/base.cpio lock=$Q_DIR/.base.lock r app
	(
		flock -x 9
		if [ -s "$base" ] && [ "$base" -nt "$NINIT" ] && [ "$base" -nt "$NINITCTL" ] &&
			[ "$base" -nt "$NINIT_SHUTDOWN" ] && [ "$base" -nt "$HELPERS/nctl-raw" ] &&
			[ "$base" -nt "$top_srcdir/tests/qemu/guest.sh" ] &&
			[ "$base" -nt "$top_srcdir/tests/qemu/lib.sh" ]; then
			exit 0
		fi
		r=$Q_DIR/base.root
		rm -rf "$r"
		mkdir -p "$r"/{bin,sbin,etc/ninit.d,proc,sys,dev,run,tmp,root,t/bin,var/log}
		chmod 1777 "$r/tmp"
		cp "$Q_BUSYBOX" "$r/bin/busybox" || exit 1
		if ldd "$Q_BUSYBOX" >/dev/null 2>&1; then
			q_copy_elf "$r" "$Q_BUSYBOX" /bin/busybox || exit 1
		fi
		for app in $("$Q_BUSYBOX" --list); do
			case $app in
			busybox | init | linuxrc | poweroff | reboot | halt | bash) continue ;;
			esac
			[ -e "$r/bin/$app" ] || ln -s busybox "$r/bin/$app"
		done
		q_copy_elf "$r" "$NINIT_SHELL" || exit 1
		q_copy_elf "$r" "$NINIT" /sbin/ninit || exit 1
		q_copy_elf "$r" "$NINITCTL" /sbin/ninitctl || exit 1
		q_copy_elf "$r" "$NINIT_SHUTDOWN" /sbin/ninit-shutdown || exit 1
		for app in shutdown poweroff reboot halt telinit; do
			ln -s ninit-shutdown "$r/sbin/$app"
		done
		q_copy_elf "$r" "$HELPERS/nctl-raw" /t/bin/nctl || exit 1
		cp "$top_srcdir/tests/qemu/guest.sh" "$r/t/lib.sh" || exit 1
		printf 'PRETTY_NAME="ninit test guest"\nID=ninit-test\n' >"$r/etc/os-release"
		printf 'root:x:0:0:root:/root:/bin/sh\nnobody:x:65534:65534:nobody:/:/bin/false\n' >"$r/etc/passwd"
		printf 'root:x:0:\ntty:x:5:\nnobody:x:65534:\n' >"$r/etc/group"
		(cd "$r" && find . | LC_ALL=C sort | "$Q_BUSYBOX" cpio -o -H newc -R 0:0 >"$base.tmp" 2>/dev/null) || exit 1
		mv "$base.tmp" "$base"
	) 9>"$lock"
}

# starts a scenario, the root overlay of the guest is $Q/root
q_new() {
	q_n=$((q_n + 1))
	Q=$T/q$q_n
	Q_NAME=$1
	mkdir -p "$Q/root/etc/ninit.d" "$Q/root/t"
	Q_ARGS=
	Q_RDINIT=/sbin/ninit
	Q_HALT=
	Q_PIPE=
	Q_NOGRAPH=
	Q_POLL=
	Q_TIMEOUT=60
	Q_EXTRA=()
}

# writes stdin to the service file NAME of the scenario
q_svc() {
	cat >"$Q/root/etc/ninit.d/$1"
}

# writes stdin to PATH in the guest
q_file() {
	mkdir -p "$Q/root$(dirname "$1")"
	cat >"$Q/root$1"
	[ -n "${2:-}" ] && chmod "$2" "$Q/root$1"
	return 0
}

# the check service runs stdin as guest assertions, then shuts the guest down by $T_END
q_check() {
	{
		printf '#%%type: daemon\n#%%onfail: warn\n#%%start-tries: 1\n'
		[ -n "${1:-}" ] && printf '#%%depon: %s\n' "$1"
		printf '. /t/lib.sh\n'
		cat
		printf '\nt_end\n'
	} >"$Q/root/etc/ninit.d/check"
}

# compiles the services of the scenario into its depgraph
q_graph() {
	local d=$Q/root/etc/ninit.d
	if ! "$NINITCTL" init -d "$d" >"$Q/init.out" 2>"$Q/init.err"; then
		t_ok 1 "$Q_NAME: the scenario graph builds"
		t_diag "$(cat "$Q/init.err")"
		return 1
	fi
	return 0
}

q_kill() {
	kill "$1" 2>/dev/null
	wait "$1" 2>/dev/null
}

# boots the scenario and waits for it to stop
q_boot() {
	q_start || return 1
	q_finish
}

# starts the guest, with Q_PIPE set the console is a fifo pair read by $Q_CAT and written by q_send
q_start() {
	local args
	[ -e "$Q/root/etc/ninit.d/depgraph" ] || [ -n "$Q_NOGRAPH" ] || q_graph || return 1
	(cd "$Q/root" && find . -mindepth 1 | LC_ALL=C sort |
		"$Q_BUSYBOX" cpio -o -H newc -R 0:0 >"$Q/scen.cpio" 2>/dev/null)
	cat "$Q_DIR/base.cpio" "$Q/scen.cpio" >"$Q/initrd"
	: >"$Q/console.raw"
	: >"$Q/tap.raw"
	args=(-nodefaults -no-user-config -display none -no-reboot
		-accel "$Q_ACCEL" -cpu "$Q_CPU" -m 384 -smp 2
		-kernel "$Q_KERNEL" -initrd "$Q/initrd"
		-append "console=$Q_CON rdinit=$Q_RDINIT panic=-1 loglevel=1 $Q_ARGS")
	if [ -n "$Q_PIPE" ]; then
		mkfifo "$Q/con.in" "$Q/con.out"
		exec {Q_CONFD}<>"$Q/con.in"
		cat <>"$Q/con.out" >>"$Q/console.raw" &
		Q_CAT=$!
		args+=(-chardev "pipe,id=con,path=$Q/con" -serial chardev:con)
	else
		args+=(-serial "file:$Q/console.raw")
	fi
	args+=(-serial "file:$Q/tap.raw" "${Q_EXTRA[@]}")
	"$Q_QEMU" "${args[@]}" </dev/null >"$Q/qemu.out" 2>&1 &
	Q_PID=$!
	Q_T0=$SECONDS
	Q_MARK=0
}

# waits for the guest to stop, then fills $q_rc, $Q/console.log and $Q/tap.log
q_finish() {
	local pid=$Q_PID t0=$Q_T0 timeout=$((Q_TIMEOUT * Q_SCALE))
	q_rc=
	while kill -0 "$pid" 2>/dev/null; do
		[ -n "${Q_POLL:-}" ] && $Q_POLL
		if [ -n "$Q_HALT" ] && grep -q 'reboot: System halted' "$Q/console.raw" 2>/dev/null; then
			sleep 0.2
			q_kill "$pid"
			q_rc=halted
			break
		fi
		if ((SECONDS - t0 > timeout)); then
			q_kill "$pid"
			q_rc=timeout
			break
		fi
		sleep 0.05
	done
	if [ -z "$q_rc" ]; then
		wait "$pid"
		q_rc=$?
	fi
	if [ -n "$Q_PIPE" ]; then
		kill -CONT "$Q_CAT" 2>/dev/null
		sleep 0.1
		q_kill "$Q_CAT"
		exec {Q_CONFD}>&-
	fi
	sed -E 's/\x1b\[[0-9;]*m//g; s/\r$//' "$Q/console.raw" >"$Q/console.log"
	tr -d '\r' <"$Q/tap.raw" >"$Q/tap.log"
	[ "$q_rc" = timeout ] && {
		t_ok 1 "$Q_NAME: the guest finishes within $timeout s"
		t_diag "$(tail -n 30 "$Q/console.log")"
	}
	return 0
}

# console output since the last q_mark
q_mark() {
	Q_MARK=$(stat -c %s "$Q/console.raw")
}

q_since() {
	tail -c +$((${Q_MARK:-0} + 1)) "$Q/console.raw" | sed -E 's/\x1b\[[0-9;]*m//g; s/\r//g'
}

# waits up to SECONDS for TEXT on the console after the last q_mark
q_wait() {
	local end=$((SECONDS + ${2:-10} * Q_SCALE))
	while ((SECONDS <= end)); do
		q_since | grep -qF -- "$1" && return 0
		kill -0 "$Q_PID" 2>/dev/null || { q_since | grep -qF -- "$1"; return; }
		sleep 0.05
	done
	return 1
}

# types TEXT on the console
q_send() {
	printf '%b' "$1" >&"$Q_CONFD"
}

# re-emits the guest tap as host tap, every guest line prefixed with the scenario name
q_import() {
	local line plan= desc
	while IFS= read -r line; do
		case $line in
		"ok "*)
			desc=${line#ok }
			desc=${desc#* - }
			t_ok 0 "$Q_NAME: $desc"
			;;
		"not ok "*)
			desc=${line#not ok }
			desc=${desc#* - }
			t_ok 1 "$Q_NAME: $desc"
			;;
		"#"*)
			printf '%s\n' "$line"
			;;
		1..*)
			plan=${line#1..}
			;;
		esac
	done <"$Q/tap.log"
	if [ -z "$plan" ]; then
		t_ok 1 "$Q_NAME: the guest checks run to the end"
		t_diag "$(tail -n 40 "$Q/console.log")"
		return 1
	fi
	return 0
}

q_console() {
	cat "$Q/console.log"
}

q_has() {
	q_visible "$1" || { t_skip "$Q_NAME: ${2:-the console shows $1}, not on a quiet console"; return 0; }
	t_has "$(q_console)" "$1" "$Q_NAME: ${2:-the console shows $1}"
}

q_lacks() {
	local hit
	q_visible "$1" || { t_skip "$Q_NAME: ${2:-the console does not show $1}, not on a quiet console"; return 0; }
	hit=$(grep -F -- "$1" "$Q/console.log")
	[ -z "$hit" ] && { t_ok 0 "$Q_NAME: ${2:-the console does not show $1}"; return 0; }
	t_ok 1 "$Q_NAME: ${2:-the console does not show $1}"
	t_diag "$hit"
	return 1
}

q_like() {
	q_visible "$1" || { t_skip "$Q_NAME: $2, not on a quiet console"; return 0; }
	t_like "$(q_console)" "$1" "$Q_NAME: $2"
}

# the first line containing A comes before the first line containing B
q_order() {
	local a b
	if ! q_visible "$1" || ! q_visible "$2"; then
		t_skip "$Q_NAME: $3, not on a quiet console"
		return 0
	fi
	a=$(grep -n -F -m1 -- "$1" "$Q/console.log" | cut -d: -f1)
	b=$(grep -n -F -m1 -- "$2" "$Q/console.log" | cut -d: -f1)
	if [ -n "$a" ] && [ -n "$b" ] && [ "$a" -lt "$b" ]; then
		t_ok 0 "$Q_NAME: $3"
		return 0
	fi
	t_ok 1 "$Q_NAME: $3"
	t_diag "'$1' at line ${a:-none}, '$2' at line ${b:-none}"
	return 1
}

# the lines ninit logged, without kernel messages
q_log() {
	grep -E '^\[[0-9]{3,}\.[0-9]{3}\] [A-Z]{4} > ' "$Q/console.log"
}

q_exit() {
	t_is "$q_rc" "${1:-0}" "$Q_NAME: qemu exits ${2:-with status 0}"
}

# the guest powered off cleanly, with every step of the shutdown sequence logged
q_poweroff_ok() {
	q_exit 0 "after poweroff"
	q_has "poweroff: now" "ninit logs poweroff: now"
	q_has "reboot: Power down" "the kernel powers down"
}
