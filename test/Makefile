CC=g++
CFLAGS=-std=gnu++20 -I../CCSVLib
OBJS=test1.o
HEADERS=

all: test1

%.o: %.cpp $(HEADERS)
	$(CC) $(CFLAGS) -I../lib -c -o $@ $<
test1: $(OBJS) $(HEADERS)
	$(CC) -L../lib -o test1 $(OBJS) -lpompeii
clean:
	rm $(OBJS)
	rm test1