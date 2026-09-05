# Copyright 2026 Gentoo Authors
# Distributed under the terms of the GNU General Public License v2

EAPI=8

inherit git-r3 toolchain-funcs

DESCRIPTION="small init"
HOMEPAGE="https://github.com/noahburchell/ninit"
EGIT_REPO_URI="https://github.com/noahburchell/ninit.git"

LICENSE="GPL-3"
SLOT="0"

IUSE="busybox debug quiet"
RESTRICT="mirror bindist"

RDEPEND="
	app-shells/bash
	busybox? ( sys-apps/busybox )
"

pkg_setup() {
	# the sources are C23 (gnu23)
	if tc-is-gcc && [[ $(gcc-major-version) -lt 14 ]]; then
		die "ninit needs gcc 14 or newer (or clang 18+) for -std=gnu23"
	fi
	if tc-is-clang && [[ $(clang-major-version) -lt 18 ]]; then
		die "ninit needs clang 18 or newer for -std=gnu23"
	fi
}

ninit_use() {
	local u=()
	use quiet && u+=( quiet )
	use busybox && u+=( busybox )
	use debug && u+=( debug )
	echo "${u[*]}"
}

src_compile() {
	emake CC="$(tc-getCC)" USE="$(ninit_use)" BUSYBOX=/bin/busybox \
		CFLAGS="${CFLAGS}" LDFLAGS="${LDFLAGS}" all
}

src_install() {
	emake CC="$(tc-getCC)" USE="$(ninit_use)" DESTDIR="${D}" PREFIX=/usr install
	keepdir /etc/ninit.d
}

pkg_postinst() {
	elog "Put service files in /etc/ninit.d and compile them with:"
	elog "    ninitctl init"
	elog "then boot with init=/usr/sbin/ninit on the kernel command line."
	elog "Shut down with: kill -USR2 1 (poweroff), kill -TERM 1 (reboot), kill -USR1 1 (halt)."
	if use debug; then
		ewarn "USE=debug builds pid 1 with sanitizers; do not run this on a real machine."
	fi
}
