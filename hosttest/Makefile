# Desktop builds of the CE engine. Compiles the same ../src/main.c.
CFLAGS  = -std=c11 -O2 -Wall -Wextra -Istub
SDLC    = $(shell pkg-config --cflags sdl2)
SDLL    = $(shell pkg-config --libs sdl2)

all: play host

play: play.c ../src/main.c $(wildcard stub/*.h)
	$(CC) $(CFLAGS) $(SDLC) -o $@ play.c $(SDLL) -lm

host: host.c ../src/main.c $(wildcard stub/*.h)
	$(CC) $(CFLAGS) -o $@ host.c -lm

# placement guard + full angle/position sweep
test: host play
	./host /dev/null
	./play --frames 600 --shot /dev/null

clean:
	rm -f play host *.ppm *.png

.PHONY: all test clean
