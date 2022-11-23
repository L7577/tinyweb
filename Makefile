CC = gcc
CFLAGC = -O2 -Wall -I .
ifeq ($(DEBUG),1)
CFLAGS = -g -DDEBUG -O2 -Wall -I .
endif
# This flag includes the Pthreads library on a Linux box.
# Others systems will probably require something different.
LIB = -lpthread

all: tiny cgi

tiny: tiny.c csapp.o sbuf.o
	$(CC) $(CFLAGS) -o tiny tiny.c csapp.o sbuf.o $(LIB)

sbuf.o: sbuf.c
	$(CC) $(CFLAGS) -c sbuf.c

csapp.o: csapp.c
	$(CC) $(CFLAGS) -c csapp.c

cgi:
	(cd cgi-bin; make)

clean:
	rm -f *.o tiny *~
	(cd cgi-bin; make clean)

