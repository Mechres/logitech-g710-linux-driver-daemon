CC = gcc
CFLAGS = -Wall -O2 $(shell pkg-config --cflags libevdev)
LIBS = $(shell pkg-config --libs libevdev)

all: g710d

default: g710d

g710d: g710d.c
	$(CC) $(CFLAGS) -o g710d g710d.c $(LIBS)

clean:
	rm -f g710d

install: g710d
	install -m 755 g710d /usr/local/bin/g710d
	install -m 644 ../../misc/g710d.service /etc/systemd/system/g710d.service
