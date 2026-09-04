CROSS_COMPILE ?=
CC      = $(CROSS_COMPILE)gcc
STRIP   = $(CROSS_COMPILE)strip
# Optimisation and hardening are tunable; a packager or distro can supply its
# own CFLAGS in the environment. The flags the code actually requires to
# compile are appended, so overriding CFLAGS cannot silently drop them.
CFLAGS ?= -O2
CFLAGS += -std=c99 -Wall -Wextra -D_GNU_SOURCE -I.
CFLAGS_STATIC = $(CFLAGS) -static
LDFLAGS ?=

DBUS_CFLAGS := $(shell pkg-config --cflags dbus-1)
DBUS_LIBS   := $(shell pkg-config --libs dbus-1)

RELDIR ?= release
BINS   ?= schema-init schema-ctl schema-subreaper schema-journal-sink schema-board schema-udev schema-dbus

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

schema-udev: schema-udev.c schema-udev.h udev_db.h udev_rules.h udev_builtins.h hwdb.h uaccess.h disk_links.h fido_id.h udev_ruleset.h path_id.h udev_exec.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< -lacl

parity: tools/udev-parity.c udev-parity.h udev_db.h udev_rules.h udev_builtins.h hwdb.h ata_id.h v4l_id.h cdrom_id.h optical_fs.h schema-udev.h fido_id.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o udev-parity tools/udev-parity.c

verify-rules-live: tools/verify-rules-live.c tools/flip_classify.h udev_db.h udev_rules.h udev_builtins.h hwdb.h udev_ruleset.h path_id.h udev_exec.h fido_id.h ata_id.h v4l_id.h cdrom_id.h optical_fs.h dissect_image.h schema-udev.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o verify-rules-live tools/verify-rules-live.c

verify-eprops-live: tools/verify-eprops-live.c udev_db.h udev_rules.h udev_builtins.h hwdb.h udev_ruleset.h path_id.h udev_exec.h fido_id.h ata_id.h v4l_id.h cdrom_id.h optical_fs.h dissect_image.h schema-udev.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o verify-eprops-live tools/verify-eprops-live.c

schema-dbus: schema-dbus.c sdbus_wire.h sdbus_policy.h sdbus_names.h sdbus_match.h sdbus_reply.h sdbus_codec.h sdbus_auth.h sdbus_conn.h sdbus_route.h sdbus_driver.h
	$(CC) $(CFLAGS) $(DBUS_CFLAGS) $(LDFLAGS) schema-dbus.c -o $@ $(DBUS_LIBS)

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

# SP1 cutover prerequisites — deploy the broker, its boot launcher, and the
# policy dissolver to the live /usr/local layout the shims use. Does NOT flip
# dbus.svc (that is the reboot-only, Jonathan-gated Step 6): after this, copy
# services/dbus.svc.sp1 over the live services/dbus.svc when ready.
install-dbus-sp1: schema-dbus
	install -d $(DESTDIR)/usr/local/bin
	install -m 0755 schema-dbus $(DESTDIR)/usr/local/bin/schema-dbus
	install -m 0755 scripts/schema-dbus-run.sh $(DESTDIR)/usr/local/bin/schema-dbus-run.sh
	install -d $(DESTDIR)/usr/local/lib/schema-init
	install -m 0755 tools/dbus-learn/dissect_policy.py $(DESTDIR)/usr/local/lib/schema-init/dissect_policy.py
	@echo
	@echo "SP1 prerequisites installed. To FLIP the bus (reboot-only, gated):"
	@echo "  cp services/dbus.svc.sp1 <live services dir>/dbus.svc  &&  reboot"

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
	rm -f $(OBJS) schema-init schema-init-static schema-ctl schema-subreaper schema-journal-sink schema-board schema-udev udev-parity libatomic_asneeded.a
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
	$(CC) $(CFLAGS) tests/test_symlink.c -o /tmp/schema-test-symlink && /tmp/schema-test-symlink
	$(CC) $(CFLAGS) tests/test_coldplug.c -o /tmp/schema-test-coldplug && /tmp/schema-test-coldplug
	$(CC) $(CFLAGS) tests/test_libudev_frame.c -o /tmp/schema-test-libudev && /tmp/schema-test-libudev
	$(CC) $(CFLAGS) tests/test_udev_db.c -o /tmp/schema-test-udevdb && /tmp/schema-test-udevdb
	$(CC) $(CFLAGS) tests/test_udev_ruleset.c -o /tmp/schema-test-ruleset && /tmp/schema-test-ruleset
	$(CC) $(CFLAGS) tests/test_udev_matcher.c -o /tmp/schema-test-matcher && /tmp/schema-test-matcher
	$(CC) $(CFLAGS) tests/test_udev_executor.c -o /tmp/schema-test-executor && /tmp/schema-test-executor
	$(CC) $(CFLAGS) tests/test_udev_r4a.c -o /tmp/schema-test-r4a && /tmp/schema-test-r4a
	$(CC) $(CFLAGS) tests/test_udev_r4b.c -o /tmp/schema-test-r4b && /tmp/schema-test-r4b
	$(CC) $(CFLAGS) tests/test_parity.c -o /tmp/schema-test-parity && /tmp/schema-test-parity
	$(CC) $(CFLAGS) tests/test_path_id.c -o /tmp/schema-test-pathid && /tmp/schema-test-pathid
	$(CC) $(CFLAGS) tests/test_usb_id.c -o /tmp/schema-test-usbid && /tmp/schema-test-usbid
	$(CC) $(CFLAGS) tests/test_input_id.c -o /tmp/schema-test-inputid && /tmp/schema-test-inputid
	$(CC) $(CFLAGS) tests/test_net_id.c -o /tmp/schema-test-netid && /tmp/schema-test-netid
	$(CC) $(CFLAGS) tests/test_blkid_pt.c -o /tmp/schema-test-blkidpt && /tmp/schema-test-blkidpt
	$(CC) $(CFLAGS) tests/test_dissect_image.c -o /tmp/schema-test-dissect && /tmp/schema-test-dissect
	$(CC) $(CFLAGS) tests/test_blkid_fs.c -o /tmp/schema-test-blkidfs && /tmp/schema-test-blkidfs
	$(CC) $(CFLAGS) tests/test_hwdb.c -o /tmp/schema-test-hwdb && /tmp/schema-test-hwdb
	$(CC) $(CFLAGS) tests/test_udev_builtins.c -o /tmp/schema-test-ub && /tmp/schema-test-ub
	$(CC) $(CFLAGS) tests/test_udev_rules.c -o /tmp/schema-test-ur && /tmp/schema-test-ur
	$(CC) $(CFLAGS) tests/test_ata_id.c -o /tmp/schema-test-ataid && /tmp/schema-test-ataid
	$(CC) $(CFLAGS) tests/test_v4l_id.c -o /tmp/schema-test-v4lid && /tmp/schema-test-v4lid
	$(CC) $(CFLAGS) tests/test_cdrom_id.c -o /tmp/schema-test-cdromid && /tmp/schema-test-cdromid
	$(CC) $(CFLAGS) tests/test_cdrom_media.c -o /tmp/schema-test-cdrommedia && /tmp/schema-test-cdrommedia
	$(CC) $(CFLAGS) tests/test_disk_links.c -o /tmp/schema-test-disklinks && /tmp/schema-test-disklinks
	$(CC) $(CFLAGS) tests/test_uaccess.c -o /tmp/schema-test-uaccess -lacl && /tmp/schema-test-uaccess
	$(CC) $(CFLAGS) tests/test_uaccess_apply.c -o /tmp/schema-test-uaccess-apply -lacl && /tmp/schema-test-uaccess-apply
	$(CC) $(CFLAGS) tests/test_sdbus_policy.c -o /tmp/schema-test-sdbus-policy && /tmp/schema-test-sdbus-policy
	$(CC) $(CFLAGS) tests/test_sdbus_conformance.c -o /tmp/schema-test-sdbus-conf && /tmp/schema-test-sdbus-conf tests/fixtures/dbus/policy-dissolved.txt tests/fixtures/dbus/policy-golden.tsv
	$(CC) $(CFLAGS) $(DBUS_CFLAGS) tests/test_sdbus_codec.c -o /tmp/schema-test-sdbus-codec $(DBUS_LIBS) && /tmp/schema-test-sdbus-codec
	$(CC) $(CFLAGS) tests/test_sdbus_names.c -o /tmp/schema-test-sdbus-names && /tmp/schema-test-sdbus-names
	$(CC) $(CFLAGS) tests/test_sdbus_match.c -o /tmp/schema-test-sdbus-match && /tmp/schema-test-sdbus-match
	$(CC) $(CFLAGS) tests/test_sdbus_reply.c -o /tmp/schema-test-sdbus-reply && /tmp/schema-test-sdbus-reply
	$(CC) $(CFLAGS) tests/test_sdbus_conn.c -o /tmp/schema-test-sdbus-conn && /tmp/schema-test-sdbus-conn
	$(CC) $(CFLAGS) tests/test_sdbus_auth.c -o /tmp/schema-test-sdbus-auth && /tmp/schema-test-sdbus-auth
	$(CC) $(CFLAGS) $(DBUS_CFLAGS) tests/test_sdbus_driver.c -o /tmp/schema-test-sdbus-driver $(DBUS_LIBS) && /tmp/schema-test-sdbus-driver
	$(CC) $(CFLAGS) $(DBUS_CFLAGS) tests/test_sdbus_route.c -o /tmp/schema-test-sdbus-route $(DBUS_LIBS) && /tmp/schema-test-sdbus-route
	$(CC) $(CFLAGS) $(DBUS_CFLAGS) tests/test_sdbus_wire.c -o /tmp/schema-test-sdbus-wire $(DBUS_LIBS) && /tmp/schema-test-sdbus-wire
	$(CC) $(CFLAGS) tests/test_sdbus_activate.c -o /tmp/schema-test-sdbus-activate && /tmp/schema-test-sdbus-activate

verify-live:
	sh tests/sdbus_live_interop.sh
	sh tests/verify_disk_links_live.sh
	sh tests/verify_uaccess_live.sh
	sh tests/verify_db_live.sh

# Local-only: prove the C policy engine byte-identical against the PRIVATE
# 14,979-msg SP0 corpus. Corpus + its derivatives never leave the machine
# (tests/dbus-corpus/* is gitignored), so this is not part of `make test`.
verify-dbus-conformance:
	cd tools/dbus-learn && python3 emit_conformance_golden.py \
	  ../../tests/dbus-corpus/capture-20260902.jsonl \
	  ../../tests/dbus-corpus/policy-dissolved-full.txt \
	  ../../tests/dbus-corpus/policy-golden-full.tsv
	$(CC) $(CFLAGS) tests/test_sdbus_conformance.c -o /tmp/schema-test-sdbus-conf-full
	/tmp/schema-test-sdbus-conf-full tests/dbus-corpus/policy-dissolved-full.txt tests/dbus-corpus/policy-golden-full.tsv

.PHONY: all clean install install-dbus-sp1 release aarch64 armhf desktop test verify-live verify-dbus-conformance


