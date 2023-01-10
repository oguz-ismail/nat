CFLAGS = -O3

nat: nat.c

test: nat
	sh ./run_tests.sh

clean:
	rm -f nat
