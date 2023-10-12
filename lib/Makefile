CC=g++
CFLAGS=-std=gnu++20
OBJS=pompeii.o
HEADERS=pompeii.h

all: libpompeii.a

%.o: %.cpp $(HEADERS)
	$(CC) $(CFLAGS) -c -o $@ $<
libpompeii.a: $(OBJS) 
	ar rcs libpompeii.a $(OBJS)
	@make -C ../test clean
clean:
	rm $(OBJS)
	rm libpompeii.a
