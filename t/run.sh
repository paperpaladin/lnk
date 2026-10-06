#!/bin/sh
# The oracle is the kernel: each case links a program out of object files and
# then runs it, asserting its exit status and its stdout. The test programs are
# freestanding -- a `_start` that makes raw `write` and `exit_group` syscalls --
# so a failure cannot be blamed on glibc's startup path.
#
# Nothing here has a hand-computed address in it. Every expectation is an exit
# status or a string the program itself chose.
cd "$(dirname "$0")/.." || exit 1

pass=0
fail=0
ok() { pass=$((pass + 1)); }
no() { fail=$((fail + 1)); shift_msg="$1"; shift; printf 'FAIL  %s\n' "$shift_msg"; for l in "$@"; do printf '        %s\n' "$l"; done; }

CF="-fno-pie -O1 -ffreestanding -fno-stack-protector"

build() {
  out="$1"; shift
  if ! gcc -c $CF -o "$out" "$@" 2> t/tmp.cc; then
    no "gcc -c failed for $out"; sed 's/^/        /' t/tmp.cc; return 1
  fi
}

# --- oracle 1: the kernel runs it, with the exit status and stdout we asked for
runs() {
  what="$1" wantrc="$2" wantout="$3"; shift 3
  if ! ./lnk -o t/tmp.bin "$@" 2> t/tmp.err; then
    no "$what: lnk refused to link" "$(cat t/tmp.err)"; return
  fi
  ./t/tmp.bin > t/tmp.out 2>/dev/null
  gotrc=$?
  gotout=$(cat t/tmp.out)
  if [ "$gotrc" != "$wantrc" ]; then
    no "$what: exit status" "expected $wantrc, got $gotrc"; return
  fi
  if [ "$gotout" != "$wantout" ]; then
    no "$what: stdout" "expected [$wantout]" "got      [$gotout]"; return
  fi
  ok
}

# ---------------------------------------------------------------------- objects
build t/exit42.o t/exit42.c || exit 1

# --------------------------------------------------------- 1. the kernel runs it
runs "single object, raw syscall _start" 42 "" t/exit42.o

printf '%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" = 0 ]
