ACLOCAL_AMFLAGS = -I m4

AM_CPPFLAGS = \
	-I$(top_builddir) \
	-I$(top_srcdir)/src \
	-include $(top_builddir)/config.h \
	-DNINIT_SBINDIR='"$(sbindir)"'

AM_CFLAGS  = @NINIT_OPT_CFLAGS@
AM_LDFLAGS = @NINIT_OPT_LDFLAGS@

# user flags may override optimisation but not warnings or hardening
COMPILE = $(CC) $(DEFS) $(DEFAULT_INCLUDES) $(INCLUDES) $(AM_CPPFLAGS) $(CPPFLAGS) \
	$(AM_CFLAGS) $(CFLAGS) $(NINIT_CPPFLAGS) $(NINIT_CFLAGS)
LINK = $(CCLD) $(AM_CFLAGS) $(CFLAGS) $(NINIT_CFLAGS) $(AM_LDFLAGS) $(LDFLAGS) $(NINIT_LDFLAGS) -o $@

sbin_PROGRAMS = ninit ninitctl ninit-shutdown

ninit_core = \
	src/boot.c \
	src/cgroup.c \
	src/command.c \
	src/control.c \
	src/drain.c \
	src/pidmap.c \
	src/reap.c \
	src/signals.c \
	src/spawn.c \
	src/state.c \
	src/stop.c \
	src/shutdown.c \
	src/ninit.c

ninit_headers = \
	src/boot.h \
	src/cgroup.h \
	src/command.h \
	src/control.h \
	src/drain.h \
	src/fail.h \
	src/logging.h \
	src/nctl.h \
	src/ngraph.h \
	src/ninit.h \
	src/pidmap.h \
	src/reap.h \
	src/signals.h \
	src/spawn.h \
	src/state.h \
	src/stop.h \
	src/shutdown.h

ninit_SOURCES = \
	$(ninit_core) \
	src/logging.c \
	src/ngraph.c \
	src/fail.c \
	$(ninit_headers)

ninitctl_parser = \
	ctl/util.c \
	ctl/parser/compact.c \
	ctl/parser/directives.c \
	ctl/parser/lang.c \
	ctl/parser/shell.c \
	ctl/parser/python.c \
	ctl/parser/lua.c \
	ctl/parser/perl.c \
	ctl/parser/lang.h \
	ctl/parser/parser.h \
	ctl/util.h

ninitctl_SOURCES = \
	ctl/main.c \
	ctl/build.c \
	ctl/check.c \
	ctl/graph.c \
	ctl/image.c \
	ctl/output.c \
	ctl/sources.c \
	ctl/show.c \
	ctl/add.c \
	ctl/del.c \
	ctl/ctl.c \
	$(ninitctl_parser) \
	src/ngraph.c \
	ctl/add.h \
	ctl/build.h \
	ctl/ctl.h \
	ctl/del.h \
	ctl/init.h \
	ctl/show.h

ninit_shutdown_SOURCES = tools/shutdown.c

NINIT_MANS = man/ninit.8 man/ninitctl.8 man/ninit-shutdown.8 man/ninit.d.5

if ENABLE_MAN
man_MANS = $(NINIT_MANS)
endif

CLEANFILES = $(NINIT_MANS)

ninit_man_edit = sed \
	-e 's|@sbindir[@]|$(sbindir)|g' \
	-e 's|@ninit_svcdir[@]|$(ninit_svcdir)|g' \
	-e 's|@ninit_shell[@]|$(ninit_shell)|g' \
	-e 's|@docdir[@]|$(docdir)|g' \
	-e 's|@PACKAGE_VERSION[@]|$(PACKAGE_VERSION)|g'

man/%: man/%.in Makefile
	$(AM_V_GEN)$(MKDIR_P) man && $(ninit_man_edit) $< > $@.tmp && mv $@.tmp $@

exampledir = $(docdir)/ninit.d
dist_example_DATA = \
	docs/ninit.d/basic \
	docs/ninit.d/binfmt \
	docs/ninit.d/bluetooth \
	docs/ninit.d/chronyd \
	docs/ninit.d/console \
	docs/ninit.d/cronie \
	docs/ninit.d/cupsd \
	docs/ninit.d/dbus \
	docs/ninit.d/elogind \
	docs/ninit.d/fs \
	docs/ninit.d/getty1 \
	docs/ninit.d/getty2 \
	docs/ninit.d/getty3 \
	docs/ninit.d/getty4 \
	docs/ninit.d/getty5 \
	docs/ninit.d/getty6 \
	docs/ninit.d/hostname \
	docs/ninit.d/hwclock \
	docs/ninit.d/loopback \
	docs/ninit.d/modules-load \
	docs/ninit.d/mounts \
	docs/ninit.d/NetworkManager \
	docs/ninit.d/sddm \
	docs/ninit.d/sshd \
	docs/ninit.d/swap \
	docs/ninit.d/sysctl \
	docs/ninit.d/sysklogd \
	docs/ninit.d/tmpfiles \
	docs/ninit.d/tmpfiles-dev \
	docs/ninit.d/udev \
	docs/ninit.d/udev-trigger

exampleunuseddir = $(docdir)/ninit.d/unused
dist_exampleunused_DATA = \
	docs/ninit.d/unused/avahi \
	docs/ninit.d/unused/dhcpcd \
	docs/ninit.d/unused/udev-settle

EXTRA_DIST = \
	README.rst \
	autogen.sh \
    LICENSE \
	$(NINIT_MANS:=.in)

unit_tests = \
	tests/unit/t-ngraph \
	tests/unit/t-logging \
	tests/unit/t-logging-quiet \
	tests/unit/t-fail \
	tests/unit/t-fail-sulogin \
	tests/unit/t-ninit-shutdown \
	tests/unit/t-build \
	tests/unit/t-ninit \
	tests/unit/t-shutdown \
	tests/unit/t-boot \
	tests/unit/t-ctlfs

ctl_tests = \
	tests/ctl/compact.test \
	tests/ctl/directives.test \
	tests/ctl/graph.test \
	tests/ctl/files.test \
	tests/ctl/interp.test \
	tests/ctl/show.test \
	tests/ctl/adddel.test \
	tests/ctl/usage.test \
	tests/ctl/docs.test \
	tests/ctl/limits.test \
	tests/ctl/lock.test \
	tests/ctl/tools.test \
	tests/ctl/client.test

qemu_tests = \
	tests/qemu/boot.test \
	tests/qemu/env.test \
	tests/qemu/fail.test \
	tests/qemu/timeout.test \
	tests/qemu/restart.test \
	tests/qemu/deps.test \
	tests/qemu/ctl.test \
	tests/qemu/output.test \
	tests/qemu/shutdown.test \
	tests/qemu/signals.test \
	tests/qemu/emergency.test \
	tests/qemu/select.test \
	tests/qemu/cgroup.test \
	tests/qemu/scale.test \
	tests/qemu/console.test \
	tests/qemu/interp.test \
	tests/qemu/fallback.test

check_PROGRAMS = $(unit_tests) tests/helpers/czdump tests/helpers/nctl-raw tests/helpers/fakectl \
	tests/helpers/deny

unit_headers = tests/unit/tap.h tests/unit/gbuild.h tests/unit/logcap.h

tests_unit_t_ngraph_SOURCES = tests/unit/t-ngraph.c $(unit_headers)
tests_unit_t_logging_SOURCES = tests/unit/t-logging.c $(unit_headers)
tests_unit_t_logging_quiet_SOURCES = $(tests_unit_t_logging_SOURCES)
tests_unit_t_logging_quiet_CPPFLAGS = $(AM_CPPFLAGS) -DNINIT_QUIET=1
tests_unit_t_fail_SOURCES = tests/unit/t-fail.c src/logging.c src/ngraph.c $(unit_headers)
tests_unit_t_fail_sulogin_SOURCES = $(tests_unit_t_fail_SOURCES)
tests_unit_t_fail_sulogin_CPPFLAGS = $(AM_CPPFLAGS) -DNINIT_SULOGIN=1 \
	-DNINIT_BUSYBOX='"/bin/busybox"'
tests_unit_t_ninit_shutdown_SOURCES = tests/unit/t-ninit-shutdown.c $(unit_headers)
tests_unit_t_build_SOURCES = tests/unit/t-build.c src/ngraph.c $(unit_headers)
tests_unit_t_ninit_SOURCES = tests/unit/t-ninit.c src/logging.c src/ngraph.c $(unit_headers)
tests_unit_t_shutdown_SOURCES = tests/unit/t-shutdown.c src/logging.c $(unit_headers)
tests_unit_t_boot_SOURCES = tests/unit/t-boot.c src/logging.c src/ngraph.c $(unit_headers)
tests_unit_t_ctlfs_SOURCES = tests/unit/t-ctlfs.c src/ngraph.c $(unit_headers)
tests_helpers_czdump_SOURCES = tests/helpers/czdump.c $(ninitctl_parser) src/ngraph.c
tests_helpers_nctl_raw_SOURCES = tests/helpers/nctl-raw.c
tests_helpers_fakectl_SOURCES = tests/helpers/fakectl.c
tests_helpers_deny_SOURCES = tests/helpers/deny.c

TESTS = $(unit_tests) $(ctl_tests) $(qemu_tests)

EXTRA_DIST += \
	$(ctl_tests) \
	$(qemu_tests) \
	tests/README.md \
	tests/lib.sh \
	tests/qemu/lib.sh \
	tests/qemu/guest.sh \
	tests/compact

TEST_EXTENSIONS = .test
LOG_DRIVER = env AM_TAP_AWK='$(AWK)' $(SHELL) $(top_srcdir)/build-aux/tap-driver.sh
TEST_LOG_DRIVER = $(LOG_DRIVER)
TEST_LOG_COMPILER = $(BASH_PROG)
ninit_test_kernel = @NINIT_TEST_KERNEL@

# a sanitizer build reports the memory a short lived ninitctl leaves to exit as a failure,
# the environment and the make command line name the test kernel before configure does
AM_TESTS_ENVIRONMENT = \
	top_srcdir='$(abs_top_srcdir)'; top_builddir='$(abs_top_builddir)'; \
	NINIT_SHELL='$(ninit_shell)'; NINIT_SHELL_NAME='$(ninit_shell_name)'; NINIT_SVCDIR='$(ninit_svcdir)'; \
	ASAN_OPTIONS=$${ASAN_OPTIONS:-detect_leaks=0}; \
	NINIT_TEST_KERNEL=$${NINIT_TEST_KERNEL:-'$(ninit_test_kernel)'}; \
	export top_srcdir top_builddir NINIT_SHELL NINIT_SHELL_NAME NINIT_SVCDIR ASAN_OPTIONS NINIT_TEST_KERNEL;

graph: ninitctl$(EXEEXT)
	$(AM_V_at)./ninitctl init -d '$(ninit_svcdir)' -o '$(ninit_svcdir)/depgraph'

install-data-hook:
	@$(MKDIR_P) "$(DESTDIR)$(ninit_svcdir)" 2>/dev/null || \
		echo "cannot create $(DESTDIR)$(ninit_svcdir)"
	@echo "install service files in $(DESTDIR)$(ninit_svcdir), then run 'ninitctl init'"
	@echo "example service files are in $(DESTDIR)$(exampledir)"

TOOL_NAMES = shutdown poweroff halt reboot telinit

tools-install: install-sbinPROGRAMS
	@d="$(DESTDIR)$(sbindir)"; \
	for n in $(TOOL_NAMES); do \
		t=$$d/$$n; \
		if test -e "$$t.old" || test -L "$$t.old"; then \
			echo "$$n.old exists, skipping"; continue; fi; \
		test -e "$$t" || test -L "$$t" || continue; \
		if test "`readlink "$$t"`" = ninit-shutdown; then continue; fi; \
		if test -L "$$t"; then \
			g=`readlink "$$t"`; b=`basename "$$g"`; \
			case " $(TOOL_NAMES) " in *" $$b "*) g=$$g.old ;; esac; \
			$(LN_S) "$$g" "$$t.old" && rm -f "$$t"; \
		else \
			mv "$$t" "$$t.old"; \
		fi; \
		echo "saved $$n -> $$n.old"; \
	done; \
	for n in $(TOOL_NAMES); do \
		rm -f "$$d/$$n" && $(LN_S) ninit-shutdown "$$d/$$n" && echo "installed $$n"; \
	done

tools-uninstall:
	@d="$(DESTDIR)$(sbindir)"; \
	for n in $(TOOL_NAMES); do \
		t=$$d/$$n; \
		if test -e "$$t.old" || test -L "$$t.old"; then \
			if test -L "$$t.old"; then \
				g=`readlink "$$t.old"`; b=`basename "$$g"`; \
				case " $(TOOL_NAMES) " in *" $${b%.old} "*) g=$${g%.old} ;; esac; \
				rm -f "$$t" && $(LN_S) "$$g" "$$t" && rm -f "$$t.old"; \
			else \
				rm -f "$$t" && mv "$$t.old" "$$t"; \
			fi; \
			echo "restored $$n"; \
		elif test "`readlink "$$t"`" = ninit-shutdown; then \
			rm -f "$$t" && echo "removed $$n"; \
		fi; \
	done; \
	rm -f "$$d/ninit-shutdown"

tools_install:   tools-install
tools_uninstall: tools-uninstall

clean-local:
	-rm -rf tests/qemu/base.cpio tests/qemu/base.cpio.tmp tests/qemu/base.root tests/qemu/smoke \
		tests/qemu/tcg.ok tests/qemu/tcg.bad tests/qemu/.base.lock tests/qemu/.tcg.lock

.PHONY: graph tools-install tools-uninstall tools_install tools_uninstall
