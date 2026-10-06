CFLAGS = -std=c11 -g -O1 -Wall -Wextra -Wno-unused-parameter -fno-common

lnk: lnk.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f lnk t/*.o t/tmp* a.out

.PHONY: clean
