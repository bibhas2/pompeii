CC=g++
CFLAGS=-std=gnu++20 -I../CCSVLib
OBJS=test1.o test2.o
HEADERS=

all: test1 test2

%.o: %.cpp $(HEADERS)
	$(CC) $(CFLAGS) -I../lib -c -o $@ $<
test1: test1.o $(HEADERS)
	$(CC) -L../lib -o test1 test1.o -lpompeii
test2: test2.o $(HEADERS)
	$(CC) -L../lib -o test2 test2.o -lpompeii
clean:
	rm $(OBJS)
	rm test1
	rm test2