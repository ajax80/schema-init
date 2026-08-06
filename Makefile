CROSS_COMPILE ?=
CC      = $(CROSS_COMPILE)gcc
STRIP   = $(CROSS_COMPILE)strip
# Optimisation and hardening are tunable; a packager or distro can supply its
# own CFLAGS in the environment. The flags the code actually requires to
# compile are appended, so overriding CFLAGS cannot silently drop them.
CFLAGS ?= -O2
CFLAGS += -std=c99 -Wall -Wextra -D_GNU_SOURCE
CFLAGS_STATIC = $(CFLAGS) -static
LDFLAGS ?=

RELDIR ?= release
BINS   ?= schema-init schema-ctl schema-subreaper schema-journal-sink schema-board schema-udev

PREFIX     ?= /usr
BINDIR     ?= $(PREFIX)/bin
DATADIR    ?= $(PREFIX)/share
SYSCONFDIR ?= /etc

ifneq ($(SYSROOT),)
  CFLAGS += --sysroot=$(SYSROOT)
endif

SRCS    = init.c schema.c service.c group.c caps.c
OBJS    = $(SRCS:.c=.o)

all: $(BINS)

desktop:
	$(MAKE) -C desktop

schema-init: $(OBJS)
	$(CC) -static -o $@ $^ -lrt

schema-init-static:
	$(CC) $(CFLAGS_STATIC) $(LDFLAGS) -o schema-init-static $(SRCS)

schema-ctl: schema-ctl.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

schema-subreaper: schema-subreaper.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

schema-journal-sink: schema-journal-sink.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

schema-board: schema-board.c schema.h schema_shm.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< -lrt

schema-udev: schema-udev.c schema-udev.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

install: all
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(BINS) $(DESTDIR)$(BINDIR)/
	install -d $(DESTDIR)$(SYSCONFDIR)/schema-init/services
	install -d $(DESTDIR)$(DATADIR)/schema-init/services
	install -m 0644 services/* $(DESTDIR)$(DATADIR)/schema-init/services/
	install -d $(DESTDIR)$(SYSCONFDIR)/logrotate.d
	install -m 0644 schema-init.logrotate $(DESTDIR)$(SYSCONFDIR)/logrotate.d/schema-init

release: all
	rm -rf $(RELDIR)
	mkdir -p $(RELDIR)
	cp $(BINS) $(RELDIR)/
	$(STRIP) $(addprefix $(RELDIR)/,$(BINS))
	cd $(RELDIR) && sha256sum $(BINS) > SHA256SUMS
	@echo
	@echo "release assets in $(RELDIR)/ (stripped):"
	@cd $(RELDIR) && ls -l $(BINS) SHA256SUMS

clean:
	rm -f $(OBJS) schema-init schema-init-static schema-ctl schema-subreaper schema-journal-sink schema-board schema-udev libatomic_asneeded.a
	rm -rf $(RELDIR)
	$(MAKE) -C desktop clean

aarch64:
	@if [ ! -f libatomic_asneeded.a ]; then ar rcs libatomic_asneeded.a; fi
	$(MAKE) CROSS_COMPILE=aarch64-linux-gnu- SYSROOT=/usr/aarch64-redhat-linux/sys-root/fc44 LDFLAGS="-L. -static" schema-init-static schema-ctl schema-subreaper schema-journal-sink

armhf:
	@if [ ! -f libatomic_asneeded.a ]; then ar rcs libatomic_asneeded.a; fi
	$(MAKE) CROSS_COMPILE=arm-linux-gnu- LDFLAGS="-L. -static" schema-init-static schema-ctl schema-subreaper schema-journal-sink

test:
	$(CC) $(CFLAGS) tests/test_reclaim.c -o /tmp/schema-test-reclaim && /tmp/schema-test-reclaim
	$(CC) $(CFLAGS) tests/test_cgroup_tiering.c -o /tmp/schema-test-tiering && /tmp/schema-test-tiering
	$(CC) $(CFLAGS) tests/test_calendar.c -o /tmp/schema-test-calendar && /tmp/schema-test-calendar
	$(CC) $(CFLAGS) tests/test_uevent_parse.c -o /tmp/schema-test-uevent && /tmp/schema-test-uevent
	$(CC) $(CFLAGS) tests/test_dev_match.c -o /tmp/schema-test-devmatch && /tmp/schema-test-devmatch
	$(CC) $(CFLAGS) tests/test_dev_load.c -o /tmp/schema-test-devload && /tmp/schema-test-devload

.PHONY: all clean install release aarch64 armhf desktop test


