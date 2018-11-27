CC = gcc
CFLAGS  = -g -Wall

all:  nat_traversal punch_server stun_server_test

# clang warn about unused argument, it requires -pthread when compiling but not when linking
nat_traversal:  main.o nat_traversal.o nat_type.o
	$(CC) $(CFLAGS) -o nat_traversal main.o nat_traversal.o nat_type.o -pthread

punch_server: punch_server.go
	go build punch_server.go

stun_server_test: stun_server_test.c
	gcc stun_host_test.c nat_type.c -o stun_host_test

main.o:  main.c
	$(CC) $(CFLAGS) -c main.c

nat_traversal.o:  nat_traversal.c
	$(CC) $(CFLAGS) -c nat_traversal.c

nat_type.o:  nat_type.c
	$(CC) $(CFLAGS) -c nat_type.c

clean:
	$(RM) stun_host_test punch_server nat_traversal *.o *~
