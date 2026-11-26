.POSIX:

CC = cc
CFLAGS = -O2 -Wall -Wextra
AS6800 = as6800
LD6800 = ld6800

all: TL1

TL1: main.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 $(LDFLAGS) -o $@ main.c $(LDLIBS)

test: TL1 FORCE
	AS6800="$(AS6800)" LD6800="$(LD6800)" sh ./test-tl1.sh

clean: FORCE
	rm -f TL1

FORCE:
