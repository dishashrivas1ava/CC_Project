CC = gcc
CFLAGS = -Wall `pkg-config fuse3 --cflags`
LIBS = `pkg-config fuse3 --libs`

all: mini_unionfs

mini_unionfs: main.c
	$(CC) $(CFLAGS) main.c -o mini_unionfs $(LIBS)

clean:
	rm -f mini_unionfs
