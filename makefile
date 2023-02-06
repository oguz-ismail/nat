CFLAGS = -O3

nat: nat.o
	$(CC) $(LDFLAGS) -o nat nat.o

test: nat
	@if command -v ksh >/dev/null 2>&1; then \
		ksh ./run_tests.sh; \
	else \
		sh ./run_tests.sh; \
	fi

clean:
	rm -f nat nat.o
