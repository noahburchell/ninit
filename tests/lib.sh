# tap helpers for the shell tests, sourced by every tests/*/*.test

: "${top_srcdir:?top_srcdir is not set, run the tests with make check}"
: "${top_builddir:?top_builddir is not set, run the tests with make check}"
: "${NINIT_SHELL:=/bin/bash}"
: "${NINIT_SHELL_NAME:=${NINIT_SHELL##*/}}"

# README 3.2, ninitctl init strips comments only for a shell that lexes like sh
case ${NINIT_SHELL_NAME##*/} in
sh | bash | dash | ash | ksh | mksh | oksh | loksh | yash | posh) t_strips=1 ;;
*) t_strips= ;;
esac

NINITCTL=$top_builddir/ninitctl
NINIT=$top_builddir/ninit
NINIT_SHUTDOWN=$top_builddir/ninit-shutdown
HELPERS=$top_builddir/tests/helpers

t_n=0
t_failed=0

T=$(mktemp -d "${TMPDIR:-/tmp}/ninit-test.XXXXXX") || exit 99
t_cleanup() {
	[ -n "${T_KEEP:-}" ] && { echo "# kept $T"; return; }
	chmod -R u+rwx "$T" 2>/dev/null
	rm -rf "$T"
}
trap t_cleanup EXIT

t_diag() {
	local l
	while IFS= read -r l; do
		printf '#   %s\n' "$l"
	done <<<"$*"
}

t_ok() {
	local rc=$1
	shift
	t_n=$((t_n + 1))
	if [ "$rc" = 0 ]; then
		printf 'ok %d - %s\n' "$t_n" "$*"
		return 0
	fi
	printf 'not ok %d - %s\n' "$t_n" "$*"
	t_failed=$((t_failed + 1))
	return 1
}

t_check() {
	local what=$1
	shift
	"$@"
	t_ok $? "$what"
}

t_is() {
	[ "$1" = "$2" ] && { t_ok 0 "$3"; return 0; }
	t_ok 1 "$3"
	t_diag "got:  $1"
	t_diag "want: $2"
	return 1
}

t_like() {
	[[ $1 =~ $2 ]] && { t_ok 0 "$3"; return 0; }
	t_ok 1 "$3"
	t_diag "got:  $1"
	t_diag "want: /$2/"
	return 1
}

t_unlike() {
	[[ $1 =~ $2 ]] || { t_ok 0 "$3"; return 0; }
	t_ok 1 "$3"
	t_diag "got:  $1"
	t_diag "not:  /$2/"
	return 1
}

t_has() {
	[[ $1 == *"$2"* ]] && { t_ok 0 "$3"; return 0; }
	t_ok 1 "$3"
	t_diag "got:  $1"
	t_diag "want: $2"
	return 1
}

t_skip() {
	t_n=$((t_n + 1))
	printf 'ok %d - %s # SKIP\n' "$t_n" "$*"
}

t_skip_all() {
	printf '1..0 # SKIP %s\n' "$*"
	exit 0
}

t_bail() {
	printf 'Bail out! %s\n' "$*"
	exit 99
}

t_done() {
	printf '1..%d\n' "$t_n"
	exit 0
}

# run CMD..., leaves stdout in $out, stderr in $err and the exit status in $rc
run() {
	"$@" >"$T/.out" 2>"$T/.err"
	rc=$?
	out=$(<"$T/.out")
	err=$(<"$T/.err")
}

# writes stdin to DIR/NAME, creating DIR
svc() {
	mkdir -p "$1" && cat >"$1/$2"
}

# the stderr lines of ninitctl that are not about /etc/locale.conf on this host
err_nolocale() {
	grep -v -e '/etc/locale.conf' -e "printf 'LANG=" <<<"$err"
}
