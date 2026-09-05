USE ?=

CC      ?= cc
PREFIX  ?= /usr
SBINDIR ?= $(PREFIX)/sbin
SVCDIR  ?= /etc/ninit.d
BUILD   ?= build
BUSYBOX ?= /bin/busybox

CC_IS_CLANG := $(shell $(CC) -dM -E -x c /dev/null 2>/dev/null | grep -c __clang__)

WARN := -Wall -Wextra -Wpedantic \
	-Wno-unused-parameter \
	-Wshadow \
	-Wundef \
	-Wcast-qual \
	-Wcast-align \
	-Wwrite-strings \
	-Wpointer-arith \
	-Wmissing-prototypes \
	-Wmissing-declarations \
	-Wstrict-prototypes \
	-Wold-style-definition \
	-Wredundant-decls \
	-Wnested-externs \
	-Wswitch-enum \
	-Wformat=2 \
	-Wvla \
	-Wdouble-promotion \
	-Wfloat-equal \
	-Winit-self \
	-Wmissing-include-dirs \
	-Wimplicit-fallthrough \
	-Wnull-dereference \
	-Wdate-time

ifeq ($(CC_IS_CLANG),0)
WARN += -Wlogical-op \
	-Wduplicated-cond \
	-Wduplicated-branches \
	-Wjump-misses-init \
	-Wtrampolines \
	-Walloc-zero \
	-Walloca \
	-Warray-bounds=2 \
	-Wshift-overflow=2 \
	-Wstringop-overflow=4 \
	-Wstringop-truncation \
	-Wformat-overflow=2 \
	-Wformat-truncation=2 \
	-Wuse-after-free=3 \
	-Wflex-array-member-not-at-end \
	-Wbidi-chars=any \
	-Wdisabled-optimization
else
WARN += -Wshadow-all \
	-Wassign-enum \
	-Wcomma \
	-Wconditional-uninitialized \
	-Wloop-analysis \
	-Wunreachable-code-aggressive \
	-Wextra-semi \
	-Wover-aligned \
	-Wformat-type-confusion \
	-Wtautological-constant-in-range-compare \
	-Wbitfield-enum-conversion \
	-Wcast-function-type-strict \
	-Wthread-safety
endif

CFLAGS   ?= -O2 -g
CFLAGS   += -std=gnu23 $(WARN)
CPPFLAGS += -D_GNU_SOURCE

ifneq (,$(filter quiet,$(USE)))
CPPFLAGS += -DNINIT_QUIET=1
endif

ifneq (,$(filter busybox,$(USE)))
CPPFLAGS += -DNINIT_BUSYBOX=\"$(BUSYBOX)\"
endif

ifneq (,$(filter debug,$(USE)))
SANFLAGS := -fsanitize=address,undefined -fsanitize-address-use-after-scope \
	-fno-sanitize-recover=all -fno-omit-frame-pointer -fno-common \
	-O0 -g3
ifneq ($(CC_IS_CLANG),0)
SANFLAGS += -fsanitize=integer,local-bounds -fno-sanitize=unsigned-integer-overflow
endif
CFLAGS  += $(SANFLAGS)
LDFLAGS += $(SANFLAGS)
endif

NINIT_SRC   := src/ninit.c src/logging.c src/ngraph.c src/fail.c
NINITCTL_SRC := ninitctl/main.c ninitctl/build.c ninitctl/show.c \
		ninitctl/add.c ninitctl/del.c src/ngraph.c

TOOLS_SRC   := tools/shutdown.c

# the names ninit-shutdown answers to, saved as NAME.old by tools_install
TOOL_NAMES := shutdown poweroff halt reboot telinit

NINIT_OBJ    := $(NINIT_SRC:%.c=$(BUILD)/obj/%.o)
NINITCTL_OBJ := $(NINITCTL_SRC:%.c=$(BUILD)/obj/%.o)
TOOLS_OBJ    := $(TOOLS_SRC:%.c=$(BUILD)/obj/%.o)
ALL_OBJ      := $(sort $(NINIT_OBJ) $(NINITCTL_OBJ) $(TOOLS_OBJ))

.PHONY: all clean install graph sanitise tools tools_install tools_uninstall FORCE

all: $(BUILD)/ninit $(BUILD)/ninitctl $(BUILD)/ninit-shutdown

tools: $(BUILD)/ninit-shutdown

$(BUILD)/ninit: $(NINIT_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

$(BUILD)/ninitctl: $(NINITCTL_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

$(BUILD)/ninit-shutdown: $(TOOLS_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

$(BUILD)/use.stamp: FORCE
	@mkdir -p $(@D)
	@echo '$(USE)' | cmp -s - $@ 2>/dev/null || { echo '$(USE)' > $@; echo "USE=$(USE)"; }

FORCE:

$(BUILD)/obj/%.o: %.c $(BUILD)/use.stamp
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DNINIT_SBINDIR=\"$(SBINDIR)\" -MMD -MP -c -o $@ $<

install: all
	install -d $(DESTDIR)$(SBINDIR) $(DESTDIR)$(SVCDIR)
	install -m755 $(BUILD)/ninit $(DESTDIR)$(SBINDIR)/ninit
	install -m755 $(BUILD)/ninitctl $(DESTDIR)$(SBINDIR)/ninitctl
	@if find $(DESTDIR)$(SVCDIR) -maxdepth 1 -type f ! -name 'depgraph*' ! -name '.*' | grep -q .; then \
		$(BUILD)/ninitctl init -d $(DESTDIR)$(SVCDIR) -o $(DESTDIR)$(SVCDIR)/depgraph; \
	else \
		echo "no services in $(DESTDIR)$(SVCDIR); add some and run 'ninitctl init'"; \
	fi

# sysvinit ships poweroff and reboot as symlinks to halt, so a plain rename
# would leave poweroff.old pointing at our own tool; a saved symlink is rewritten
# to name the saved target instead
tools_install: $(BUILD)/ninit-shutdown
	install -d $(DESTDIR)$(SBINDIR)
	install -m755 $(BUILD)/ninit-shutdown $(DESTDIR)$(SBINDIR)/ninit-shutdown
	@d=$(DESTDIR)$(SBINDIR); \
	for n in $(TOOL_NAMES); do \
		t=$$d/$$n; \
		if [ -e $$t.old ] || [ -L $$t.old ]; then echo "$$n.old already saved, leaving it"; continue; fi; \
		[ -e $$t ] || [ -L $$t ] || continue; \
		if [ "`readlink $$t`" = ninit-shutdown ]; then continue; fi; \
		if [ -L $$t ]; then \
			g=`readlink $$t`; b=`basename "$$g"`; \
			case " $(TOOL_NAMES) " in *" $$b "*) g=$$g.old ;; esac; \
			ln -s "$$g" $$t.old && rm -f $$t; \
		else \
			mv $$t $$t.old; \
		fi; \
		echo "saved $$n -> $$n.old"; \
	done; \
	for n in $(TOOL_NAMES); do ln -sfn ninit-shutdown $$d/$$n && echo "installed $$n"; done

tools_uninstall:
	@d=$(DESTDIR)$(SBINDIR); \
	for n in $(TOOL_NAMES); do \
		t=$$d/$$n; \
		if [ -e $$t.old ] || [ -L $$t.old ]; then \
			if [ -L $$t.old ]; then \
				g=`readlink $$t.old`; b=`basename "$$g"`; \
				case " $(TOOL_NAMES) " in *" $${b%.old} "*) g=$${g%.old} ;; esac; \
				rm -f $$t && ln -s "$$g" $$t && rm -f $$t.old; \
			else \
				rm -f $$t && mv $$t.old $$t; \
			fi; \
			echo "restored $$n"; \
		fi; \
	done; \
	rm -f $$d/ninit-shutdown

graph: $(BUILD)/ninitctl
	$(BUILD)/ninitctl init -d $(SVCDIR) -o $(SVCDIR)/depgraph

sanitise:
	$(MAKE) USE="$(USE) debug" all

clean:
	rm -rf $(BUILD)

-include $(ALL_OBJ:.o=.d)
