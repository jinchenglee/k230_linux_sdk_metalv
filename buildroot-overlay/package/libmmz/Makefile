CFLAG := -mcpu=c908 -O2

all:
	$(CC) $(CFLAG) -c mmz.c -o mmz.o
	$(AR) rcs libmmz.a mmz.o

clean:
	rm -f *.o
