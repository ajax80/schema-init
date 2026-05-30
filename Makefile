CC      = gcc
CFLAGS  = -std=c99 -Wall -Wextra -O2 -D_GNU_SOURCE
CFLAGS_STATIC = $(CFLAGS) -static

SRCS    = init.c schema.c service.c group.c
OBJS    = $(SRCS:.c=.o)

all: schema-init schema-ctl schema-subreaper

schema-init: $(OBJS)
	$(CC) -static -o $@ $^ -lrt

schema-init-static: $(OBJS)
	$(CC) $(CFLAGS_STATIC) -o $@ $(SRCS)

schema-ctl: schema-ctl.c
	$(CC) $(CFLAGS) -o $@ $<

schema-subreaper: schema-subreaper.c
	$(CC) $(CFLAGS) -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) schema-init schema-init-static schema-ctl schema-subreaper

.PHONY: all clean
