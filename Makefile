CROSS_COMPILE ?=
CC      = $(CROSS_COMPILE)gcc
STRIP   = $(CROSS_COMPILE)strip
CFLAGS  = -std=c99 -Wall -Wextra -O2 -D_GNU_SOURCE
CFLAGS_STATIC = $(CFLAGS) -static
LDFLAGS ?=

RELDIR ?= release
BINS   ?= schema-init schema-ctl schema-subreaper schema-journal-sink

ifneq ($(SYSROOT),)
  CFLAGS += --sysroot=$(SYSROOT)
endif

SRCS    = init.c schema.c service.c group.c
OBJS    = $(SRCS:.c=.o)

all: schema-init schema-ctl schema-subreaper schema-journal-sink

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

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

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
	rm -f $(OBJS) schema-init schema-init-static schema-ctl schema-subreaper schema-journal-sink libatomic_asneeded.a
	rm -rf $(RELDIR)
	$(MAKE) -C desktop clean

aarch64:
	@if [ ! -f libatomic_asneeded.a ]; then ar rcs libatomic_asneeded.a; fi
	$(MAKE) CROSS_COMPILE=aarch64-linux-gnu- SYSROOT=/usr/aarch64-redhat-linux/sys-root/fc44 LDFLAGS="-L. -static" schema-init-static schema-ctl schema-subreaper schema-journal-sink

armhf:
	@if [ ! -f libatomic_asneeded.a ]; then ar rcs libatomic_asneeded.a; fi
	$(MAKE) CROSS_COMPILE=arm-linux-gnu- LDFLAGS="-L. -static" schema-init-static schema-ctl schema-subreaper schema-journal-sink

.PHONY: all clean release aarch64 armhf desktop


