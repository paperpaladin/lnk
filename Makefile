CFLAGS = -std=c11 -g -O1 -Wall -Wextra -Wno-unused-parameter -fno-common

lnk: lnk.c
	$(CC) $(CFLAGS) -o $@ $<

test: lnk
	@./t/run.sh

clean:
	rm -f lnk t/*.o t/tmp* a.out

.PHONY: test clean
