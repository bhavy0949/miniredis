CC      = cc
CFLAGS  = -Wall -Wextra -O2 -g -std=c11
OBJ     = server.o dict.o

miniredis: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ)

server.o: server.c dict.h
dict.o:   dict.c dict.h

run: miniredis
	./miniredis

clean:
	rm -f miniredis *.o

.PHONY: run clean
