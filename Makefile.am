ACLOCAL_AMFLAGS = -I m4

AM_CPPFLAGS = \
	-I$(top_builddir) \
	-I$(top_srcdir)/src \
	-include $(top_builddir)/config.h \
	-DNINIT_SBINDIR='"$(sbindir)"' \
	@NINIT_CPPFLAGS@

AM_CFLAGS  = @NINIT_CFLAGS@
AM_LDFLAGS = @NINIT_LDFLAGS@

sbin_PROGRAMS = ninit ninitctl ninit-shutdown

ninit_SOURCES = \
	src/ninit.c \
	src/logging.c \
	src/ngraph.c \
	src/fail.c \
	src/fail.h \
	src/logging.h \
	src/nctl.h \
	src/ngraph.h

ninitctl_SOURCES = \
	ctl/main.c \
	ctl/build.c \
	ctl/show.c \
	ctl/add.c \
	ctl/del.c \
	ctl/ctl.c \
	src/ngraph.c \
	ctl/add.h \
	ctl/build.h \
	ctl/ctl.h \
	ctl/del.h \
	ctl/show.h

ninit_shutdown_SOURCES = tools/shutdown.c

EXTRA_DIST = \
	README.md \
	autogen.sh \
	build-aux/progress \
    LICENSE \
	docs/ninit.d

NINIT_RUN   := $(abs_top_builddir)/.ninit-progress/$(shell echo $$PPID)
NINIT_STEPS  = $(sort $(ninit_OBJECTS) $(ninitctl_OBJECTS) $(ninit_shutdown_OBJECTS)) \
	       ninit$(EXEEXT) ninitctl$(EXEEXT) ninit-shutdown$(EXEEXT)
NINIT_TOTAL  = $(words $(NINIT_STEPS))
NINIT_SAY    = $(SHELL) $(top_srcdir)/build-aux/progress '$(NINIT_RUN)' '$(NINIT_TOTAL)' '$@'

ninit_v_dep  = $(dir $@)$(DEPDIR)/$(basename $(notdir $@)).Tpo
ninit_v_prog = $(subst -,_,$(basename $@))

am__v_CC_0   = @$(NINIT_SAY) $(COMPILE) -MT $@ -MD -MP -MF $(ninit_v_dep) -c -o $@ $<;
am__v_CCLD_0 = @$(NINIT_SAY) $(LINK) $($(ninit_v_prog)_OBJECTS) $($(ninit_v_prog)_LDADD) $(LIBS);

graph: ninitctl$(EXEEXT)
	$(AM_V_at)./ninitctl init -d '$(ninit_svcdir)' -o '$(ninit_svcdir)/depgraph'

install-data-hook:
	@$(MKDIR_P) "$(DESTDIR)$(ninit_svcdir)" 2>/dev/null || \
		echo "cannot create $(DESTDIR)$(ninit_svcdir)"
	@echo "install service files in $(DESTDIR)$(ninit_svcdir), then run 'ninitctl init'"

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
		fi; \
	done; \
	rm -f "$$d/ninit-shutdown"

tools_install:   tools-install
tools_uninstall: tools-uninstall

clean-local:
	-rm -rf $(abs_top_builddir)/.ninit-progress

.PHONY: graph tools-install tools-uninstall tools_install tools_uninstall
