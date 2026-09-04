USE ?=

CC      ?= cc
PREFIX  ?= /usr
SBINDIR ?= $(PREFIX)/sbin
CONFDIR ?= /etc/ninit
SVCDIR  ?= $(CONFDIR)/ninit.d
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

NINIT_SRC   := src/init.c src/logging.c src/ngraph.c src/fail.c
NINITCTL_SRC := ninitctl/main.c ninitctl/build.c ninitctl/show.c src/ngraph.c

NINIT_OBJ    := $(NINIT_SRC:%.c=$(BUILD)/obj/%.o)
NINITCTL_OBJ := $(NINITCTL_SRC:%.c=$(BUILD)/obj/%.o)
ALL_OBJ      := $(sort $(NINIT_OBJ) $(NINITCTL_OBJ))

.PHONY: all clean install graph sanitise FORCE

all: $(BUILD)/ninit $(BUILD)/ninitctl

$(BUILD)/ninit: $(NINIT_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

$(BUILD)/ninitctl: $(NINITCTL_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

$(BUILD)/use.stamp: FORCE
	@mkdir -p $(@D)
	@echo '$(USE)' | cmp -s - $@ 2>/dev/null || { echo '$(USE)' > $@; echo "USE=$(USE)"; }

FORCE:

$(BUILD)/obj/%.o: %.c $(BUILD)/use.stamp
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

install: all
	install -d $(DESTDIR)$(SBINDIR) $(DESTDIR)$(SVCDIR)
	install -m755 $(BUILD)/ninit $(DESTDIR)$(SBINDIR)/ninit
ifneq (,$(filter ctl,$(USE)))
	install -m755 $(BUILD)/ninitctl $(DESTDIR)$(SBINDIR)/ninitctl
endif
	$(BUILD)/ninitctl init -d $(DESTDIR)$(SVCDIR) -o $(DESTDIR)$(CONFDIR)/depgraph

graph: $(BUILD)/ninitctl
	$(BUILD)/ninitctl init -d $(SVCDIR) -o $(CONFDIR)/depgraph

sanitise:
	$(MAKE) USE="$(USE) debug" all

clean:
	rm -rf $(BUILD)

-include $(ALL_OBJ:.o=.d)
