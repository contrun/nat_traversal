CC = gcc
# clang warn about unused argument, it requires -pthread when compiling but not when linking
CFLAGS  = -g -Wall -pthread
DEBUG_CFLAGS  = -g -Wall -pthread -D VERBOSE

all-debug: nat_traversal-debug punch_server stun_host_test

all:  nat_traversal punch_server stun_host_test

nat_traversal-debug: main.c nat_traversal.c nat_type.o utils.c
	$(CC) $(DEBUG_CFLAGS) -o nat_traversal main.c nat_traversal.c nat_type.c utils.c

nat_traversal: main.c nat_traversal.c nat_type.c utils.c
	$(CC) $(CFLAGS) -o nat_traversal main.c nat_traversal.c nat_type.c utils.c

punch_server: punch_server.go
	go build punch_server.go

stun_host_test: stun_host_test.c nat_type.c
	gcc stun_host_test.c nat_type.c -o stun_host_test

clean:
	$(RM) stun_host_test punch_server nat_traversal *.o *~
