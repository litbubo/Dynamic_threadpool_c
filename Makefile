CFLAGS+=-Wall
LDFLAGS+=-pthread
OBJ:=main.o Threadpool.o

all:threadpool

threadpool : $(OBJ)
	gcc $(OBJ) -o $@ $(CFLAGS) $(LDFLAGS)

%.o : %.c
	gcc -c $< -o $@ $(CFLAGS) $(LDFLAGS)

clean:
	rm -rf *.o threadpool
	
print:
	echo $(OBJ)
	echo $(LDFLAGS)