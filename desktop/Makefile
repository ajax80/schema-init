CC     = gcc
CFLAGS = -std=c99 -Wall -O2 $(shell pkg-config --cflags sdl2 SDL2_ttf)
LIBS   = $(shell pkg-config --libs sdl2 SDL2_ttf) -lrt

schema-desktop: schema-desktop.c ../schema_shm.h ../schema.h
	$(CC) $(CFLAGS) -o $@ schema-desktop.c $(LIBS)

clean:
	rm -f schema-desktop

.PHONY: clean
