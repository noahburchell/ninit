# guest side of the qemu tests, installed as /t/lib.sh and sourced by the check service
# results go to the second serial port as tap, T_END selects how the guest stops afterwards

PATH=/t/bin:$PATH
t_tty=
if [ -c /dev/ttyS1 ]; then
	stty -F /dev/ttyS1 raw -echo 2>/dev/null
	exec 9>/dev/ttyS1
	t_tty=1
else
	exec 9>/dev/console
fi

t_n=0
t_bytes=0

t_emit() {
	local LC_ALL=C line="$*"
	printf '%s\n' "$line" >&9
	t_bytes=$((t_bytes + ${#line} + 1))
}

# bytes ttyS1 has sent, a write only queues them and a poweroff would lose the rest
t_sent() {
	awk '$1 == "1:" { for (k = 2; k <= NF; k++) if ($k ~ /^tx:/) print substr($k, 4) }' \
		/proc/tty/driver/serial 2>/dev/null
}

t_flushed() {
	local tx
	tx=$(t_sent)
	[ -z "$tx" ] || [ "$tx" -ge "$t_bytes" ]
}

diag() {
	local l
	while IFS= read -r l; do
		t_emit "#   $l"
	done <<<"$*"
}

ok() {
	local rc=$1
	shift
	t_n=$((t_n + 1))
	if [ "$rc" = 0 ]; then
		t_emit "ok $t_n - $*"
		return 0
	fi
	t_emit "not ok $t_n - $*"
	return 1
}

check() {
	local what=$1
	shift
	"$@"
	ok $? "$what"
}

is() {
	[ "$1" = "$2" ] && { ok 0 "$3"; return 0; }
	ok 1 "$3"
	diag "got:  $1"
	diag "want: $2"
	return 1
}

like() {
	[[ $1 =~ $2 ]] && { ok 0 "$3"; return 0; }
	ok 1 "$3"
	diag "got:  $1"
	diag "want: /$2/"
	return 1
}

unlike() {
	[[ $1 =~ $2 ]] || { ok 0 "$3"; return 0; }
	ok 1 "$3"
	diag "got:  $1"
	diag "not:  /$2/"
	return 1
}

has() {
	[[ $1 == *"$2"* ]] && { ok 0 "$3"; return 0; }
	ok 1 "$3"
	diag "got:  $1"
	diag "want: $2"
	return 1
}

lacks() {
	[[ $1 == *"$2"* ]] || { ok 0 "$3"; return 0; }
	ok 1 "$3"
	diag "got:  $1"
	return 1
}

now_ms() {
	local t=${EPOCHREALTIME//[.,]/}
	echo $((t / 1000))
}

# the status line of NAME as ". NAME STATE HOLD pid PID"
st_line() {
	nctl "status $1" | head -n1
}

# STATE of NAME
st() {
	local l
	l=$(st_line "$1")
	l=${l#. * }
	echo "${l%% *}"
}

# main pid of NAME, 0 when it has none
pid_of() {
	local l
	l=$(st_line "$1")
	echo "${l##* }"
}

# waits up to MS for NAME to reach STATE
wait_st() {
	local end=$(($(now_ms) + ${3:-10000}))
	while [ "$(st "$1")" != "$2" ]; do
		(($(now_ms) > end)) && return 1
		sleep 0.02
	done
	return 0
}

# checks that NAME reaches STATE within MS
st_is() {
	local what=${4:-$1 is $2}
	if wait_st "$1" "$2" "${3:-10000}"; then
		ok 0 "$what"
		return 0
	fi
	ok 1 "$what"
	diag "$(st_line "$1")"
	return 1
}

# waits up to MS for CMD to succeed
wait_for() {
	local ms=$1 end
	shift
	end=$(($(now_ms) + ms))
	until "$@"; do
		(($(now_ms) > end)) && return 1
		sleep 0.02
	done
	return 0
}

# the log ring as ninitctl log prints it
ring() {
	ninitctl log
}

ring_has() {
	ring | grep -qF -- "$1"
}

# waits up to MS for TEXT in the log ring
wait_log() {
	wait_for "${2:-10000}" ring_has "$1"
}

log_has() {
	if wait_log "$1" "${3:-10000}"; then
		ok 0 "${2:-the log shows $1}"
		return 0
	fi
	ok 1 "${2:-the log shows $1}"
	diag "$(ring | tail -n 15)"
	return 1
}

# the cgroup of the service with index I
cg_of() {
	echo "/sys/fs/cgroup/ninit.services/svc-$1"
}

# index of NAME in the running graph
idx_of() {
	ninitctl show | awk -v n="$1" '$2 == n { print $1; exit }'
}

t_end() {
	t_emit "1..$t_n"
	[ -n "$t_tty" ] && wait_for 3000 t_flushed
	sync
	case ${T_END:-poweroff} in
	poweroff) kill -USR2 1 ;;
	reboot) kill -TERM 1 ;;
	halt) kill -USR1 1 ;;
	none) ;;
	*) $T_END ;;
	esac
	exec sleep 100000
}
