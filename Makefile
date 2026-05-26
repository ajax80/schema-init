CC      = gcc
CFLAGS  = -std=c99 -Wall -Wextra -O2 -D_GNU_SOURCE
CFLAGS_STATIC = $(CFLAGS) -static

SRCS    = init.c schema.c service.c
OBJS    = $(SRCS:.c=.o)

all: schema-init

schema-init: $(OBJS)
	$(CC) -o $@ $^

schema-init-static: $(OBJS)
	$(CC) $(CFLAGS_STATIC) -o $@ $(SRCS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) schema-init schema-init-static

.PHONY: all clean
