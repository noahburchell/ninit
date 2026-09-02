USE ?=

CC      ?= cc
PREFIX  ?= /usr
SBINDIR ?= $(PREFIX)/sbin
CONFDIR ?= /etc/ninit
SVCDIR  ?= $(CONFDIR)/ninit.d
BUILD   ?= build

CFLAGS   ?= -O2 -g
CFLAGS   += -std=gnu23 -Wall -Wextra -Wno-unused-parameter
CPPFLAGS += -D_GNU_SOURCE

ifneq (,$(filter quiet,$(USE)))
CPPFLAGS += -DNINIT_QUIET=1
endif

NINIT_SRC   := src/init.c src/logging.c src/ngraph.c src/fail.c
NINITCTL_SRC := ninitctl/main.c ninitctl/build.c ninitctl/show.c src/ngraph.c

NINIT_OBJ    := $(NINIT_SRC:%.c=$(BUILD)/obj/%.o)
NINITCTL_OBJ := $(NINITCTL_SRC:%.c=$(BUILD)/obj/%.o)
ALL_OBJ      := $(sort $(NINIT_OBJ) $(NINITCTL_OBJ))

.PHONY: all clean install graph FORCE

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

clean:
	rm -rf $(BUILD)

-include $(ALL_OBJ:.o=.d)
