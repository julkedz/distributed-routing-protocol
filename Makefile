CC = gcc
CFLAGS = -std=c17 -Wall -Wextra

all: router

router: router.o
	$(CC) $(CFLAGS) router.o -o router

router.o: router.c
	$(CC) $(CFLAGS) -c router.c

clean:
	rm -f *.o

distclean: clean
	rm -f router
