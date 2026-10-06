CFLAGS = -std=c11 -g -O1 -Wall -Wextra -Wno-unused-parameter -fno-common

lnk: lnk.c
	$(CC) $(CFLAGS) -o $@ $<

test: lnk
	@./t/run.sh

# The inputs are untrusted object files, so the parser gets the whole suite run
# through it under the sanitizers, not just the happy path. Built under a
# separate name so a sanitizer binary can never be left behind as ./lnk.
lnk-asan: lnk.c
	$(CC) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=all -o $@ $<

test-asan: lnk-asan
	@LNK=./lnk-asan ./t/run.sh

clean:
	rm -f lnk lnk-asan t/*.o t/tmp* a.out

.PHONY: test test-asan clean
