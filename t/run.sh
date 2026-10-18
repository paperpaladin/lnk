#!/bin/sh
# The oracle, in order of how little it can be argued with:
#
#   1. the kernel executes the output, or it does not  -> runs()
#   2. the system ld links the same objects to a program that behaves
#      identically                                     -> agrees()
#   3. readelf accepts the headers, and the two things that make a hand-written
#      linker emit an instant-segfault binary are asserted directly: segment
#      vaddr == offset (mod page), and p_filesz <= p_memsz   -> validates()
#   4. bad input is refused, by name, rather than linked to zero -> rejects()
#
# Nothing here has a hand-computed address in it. Every expectation is either an
# exit status the program itself chose, or whatever ld did.
cd "$(dirname "$0")/.." || exit 1

# Scratch paths carry the pid, and are cleaned up on the way out. They were
# fixed names, so two runs of this suite at once overwrote each other's object
# files and linked whichever won -- failures that belong to neither run. Hit
# for real: two instances of .portfolio/extract.py overlapped and the suites
# with fixed scratch names reported failures they do not have.
#
# Nothing used to delete these, either, which is how t/tmp.pages.c came to be
# sitting in the tree permanently. 2,456 lines of generated C that the
# portfolio's line counter walked into and credited to this repo, making its
# test count look like 2,873 lines instead of 353.
T=t/tmp.$$
trap 'rm -f $T*' EXIT INT TERM

# make test-asan points this at the sanitizer build instead.
LNK="${LNK:-./lnk}"

pass=0
fail=0
ok() { pass=$((pass + 1)); }
no() { fail=$((fail + 1)); shift_msg="$1"; shift; printf 'FAIL  %s\n' "$shift_msg"; for l in "$@"; do printf '        %s\n' "$l"; done; }

CF="-fno-pie -O1 -ffreestanding -fno-stack-protector"

build() {
  out="$1"; shift
  if ! gcc -c $CF -o "$out" "$@" 2> $T.cc; then
    no "gcc -c failed for $out"; sed 's/^/        /' $T.cc; return 1
  fi
}

# --- oracle 1: the kernel runs it, with the exit status and stdout we asked for
runs() {
  what="$1" wantrc="$2" wantout="$3"; shift 3
  if ! "$LNK" -o $T.bin "$@" 2> $T.err; then
    no "$what: lnk refused to link" "$(cat $T.err)"; return
  fi
  ./$T.bin > $T.out 2>/dev/null
  gotrc=$?
  gotout=$(cat $T.out)
  if [ "$gotrc" != "$wantrc" ]; then
    no "$what: exit status" "expected $wantrc, got $gotrc"; return
  fi
  if [ "$gotout" != "$wantout" ]; then
    no "$what: stdout" "expected [$wantout]" "got      [$gotout]"; return
  fi
  ok
}

# --- oracle 2: the system linker is the reference implementation
agrees() {
  what="$1"; shift
  entry=""
  case "$1" in -e) entry="-e $2"; shift 2;; esac
  if ! "$LNK" -o $T.a $entry "$@" 2> $T.err; then
    no "$what vs ld: lnk refused to link" "$(cat $T.err)"; return
  fi
  if ! ld -o $T.b $entry "$@" 2> $T.err; then
    no "$what vs ld: ld refused to link" "$(cat $T.err)"; return
  fi
  ./$T.a > $T.oa 2>/dev/null; ra=$?
  ./$T.b > $T.ob 2>/dev/null; rb=$?
  if [ "$ra" != "$rb" ]; then
    no "$what vs ld: exit status" "lnk gave $ra, ld gave $rb"; return
  fi
  if ! cmp -s $T.oa $T.ob; then
    no "$what vs ld: stdout differs" "$(cmp $T.oa $T.ob 2>&1)"; return
  fi
  ok
}

# --- oracle 3: readelf accepts it, and the layout invariants hold
validates() {
  what="$1" f="$2"
  for opt in -h -l -S; do
    if ! readelf "$opt" "$f" > $T.re 2> $T.ree; then
      no "$what: readelf $opt exited nonzero" "$(cat $T.ree)"; return
    fi
    if [ -s $T.ree ]; then
      no "$what: readelf $opt wrote to stderr" "$(cat $T.ree)"; return
    fi
    if grep -Eq 'Error|Warning|corrupt|unexpected|<unknown' $T.re; then
      no "$what: readelf $opt complained" "$(grep -E 'Error|Warning|corrupt|unexpected|<unknown' $T.re)"; return
    fi
  done
  # Program-header invariants, read back out of the file we just wrote.
  # The checker prints OK on success, and its absence is a failure.
  #
  # This captured stdout only and tested `[ -n "$msg" ]`, so it was fail-open:
  # a missing python3, or any exception inside the snippet -- readelf format
  # drift giving an IndexError, say -- writes to stderr, leaves $msg empty,
  # and the assertion passed. Replacing python3 with a name that does not
  # exist produced four "not found" messages and then "29 passed, 0 failed".
  # This is PLAN oracle 4 and the oracle that caught the only bug this
  # project records, so it is the last one that should be fail-open. Its
  # companion .bss check is fail-closed, which is the asymmetry that gave it
  # away.
  msg=$(readelf -lW "$f" 2>&1 | python3 -c '
import sys
load = [l.split() for l in sys.stdin if l.split()[:1] == ["LOAD"]]
bad = []
if not load:
    bad.append("no LOAD segments")
for f in load:
    off, va, fs, ms = (int(f[i], 16) for i in (1, 2, 4, 5))
    if (va - off) % 0x1000:
        bad.append("vaddr %#x and offset %#x are not congruent mod 0x1000" % (va, off))
    if fs > ms:
        bad.append("p_filesz %#x > p_memsz %#x" % (fs, ms))
print("OK" if not bad else "; ".join(bad))
' 2>&1)
  if [ "$msg" != OK ]; then no "$what: ${msg:-the layout checker produced nothing}"; return; fi
  ok
}

# --- oracle 4: refused by name, never silently linked to zero
rejects() {
  what="$1" pat="$2"; shift 2
  "$LNK" -o $T.bin "$@" > $T.err 2>&1
  rc=$?
  if [ "$rc" -eq 0 ]; then
    no "$what: lnk exited 0, but should have refused"; return
  fi
  # Exit 1 exactly, and no sanitizer output. The status was only tested for
  # being nonzero and nothing ever looked for AddressSanitizer or "runtime
  # error" in $T.err -- 10 of the 29 assertions are rejects(), so a memory
  # error in any refusal path could not fail `make test-asan`. Replacing
  # die()'s exit(1) with a null-pointer store left 29 passed, 0 failed under
  # both make test and make test-asan, while the binary segfaulted on every
  # refusal.
  if grep -qE 'AddressSanitizer|runtime error|LeakSanitizer' $T.err; then
    no "$what: a sanitizer fired while refusing" "$(head -3 $T.err)"; return
  fi
  if [ "$rc" -ne 1 ]; then
    no "$what: exited $rc, not a clean refusal (1)" "$(head -3 $T.err)"; return
  fi
  if ! grep -q "$pat" $T.err; then
    no "$what: error message does not mention '$pat'" "$(cat $T.err)"; return
  fi
  ok
}

# ---------------------------------------------------------------------- objects
build t/exit42.o t/exit42.c || exit 1

# The smallest possible cross-object call, generated rather than checked in:
# one object calls a function the other defines, and nothing else happens.
printf 'int addone(int);\nstatic void sys_exit(long c){__asm__ volatile("syscall"::"a"(60L),"D"(c):"memory");__builtin_unreachable();}\nvoid _start(void){ sys_exit(addone(41)); }\n' > $T.callA.c
printf 'int addone(int x){ return x + 1; }\n' > $T.callB.c
build $T.callA.o $T.callA.c || exit 1
build $T.callB.o $T.callB.c || exit 1

build t/main.o   t/main.c   || exit 1
build t/lib.o    t/lib.c    || exit 1
build t/weak.o   t/weak.c   || exit 1
build t/weak2.o  t/weak2.c  || exit 1

gcc -c -o t/asm.o t/asm.s || no "gcc -c t/asm.s"

build t/undef.o  t/undef.c  || exit 1
build t/dup.o    t/dup.c    || exit 1

# -fPIC on purpose, and without $CF because its -fno-pie would cancel it: this
# object carries a GOT-based relocation that lnk must refuse by name.
gcc -c -fPIC -O1 -o t/gotpc.o t/gotpc.c || no "gcc -c -fPIC t/gotpc.c"

# A text section bigger than one page, and more than a page of initialised data,
# generated rather than checked in: the interesting part is the page crossing,
# not the source.
{
  echo 'int chain(int x) {'
  i=0; while [ $i -lt 400 ]; do echo "  x += $i; __asm__ volatile(\"nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop\");"; i=$((i + 1)); done
  echo '  return x; }'
  echo 'int wide[2048] = {'
  i=0; while [ $i -lt 2048 ]; do echo "$((i % 97)),"; i=$((i + 1)); done
  echo '0};'
  echo 'int widebss[8192];'
  echo 'static void sys_exit(long c){__asm__ volatile("syscall"::"a"(60L),"D"(c):"memory");__builtin_unreachable();}'
  echo 'void page_entry(void){ long s=0; for(int i=0;i<2048;i++) s+=wide[i];'
  echo '  for(int i=0;i<8192;i++) s+=widebss[i]; s+=chain(0); sys_exit(s%251); }'
} > $T.pages.c
build t/pages.o $T.pages.c || exit 1

# An object with no .note.GNU-stack, to exercise the exec-stack path.
printf '\t.text\n\t.globl nostack_fn\nnostack_fn:\n\tret\n' > $T.nostack.s
as -o $T.nostack.o $T.nostack.s || no "as $T.nostack.s"

# --------------------------------------------------------- 1. the kernel runs it
runs "single object, raw syscall _start" 42 "" t/exit42.o

runs "a call across two objects" 42 "" $T.callA.o $T.callB.o
runs "the same two in reverse order" 42 "" $T.callB.o $T.callA.o

runs "three objects, cross-object calls and data" 42 "hello from lib
42" t/main.o t/lib.o t/asm.o
runs "the same three in reverse order" 42 "hello from lib
42" t/asm.o t/lib.o t/main.o
runs "local/weak/strong resolution" 13 "" t/weak.o t/weak2.o
runs "weak twin seen before the strong one" 13 "" t/weak2.o t/weak.o

# -e names a different entry point, and the default _start is absent entirely.
if "$LNK" -o $T.bin -e page_entry t/pages.o 2> $T.err; then
  ./$T.bin; rc=$?
  if [ "$rc" = 174 ]; then ok; else no "-e page_entry: exit status" "expected 174, got $rc"; fi
else
  no "-e page_entry: lnk refused" "$(cat $T.err)"
fi
rejects "default entry _start is required" "_start" t/pages.o

# ------------------------------------------------------ 2. ld is the reference
agrees "three objects"        t/main.o t/lib.o t/asm.o
agrees "reverse link order"   t/asm.o t/lib.o t/main.o
agrees "weak and local names" t/weak.o t/weak2.o
agrees "multi-page text and data" -e page_entry t/pages.o
agrees "single object"        t/exit42.o


# The regression for commit 15: an alignment larger than the 0x400000 load base
# used to break the vaddr/offset congruence and produce a segment header that
# did not cover its own section. ld gets this right, so ld is the assertion.
printf 'int tbl[4] __attribute__((aligned(0x800000))) = {1,2,3,4};\nstatic void sys_exit(long c){__asm__ volatile("syscall"::"a"(60L),"D"(c):"memory");__builtin_unreachable();}\nvoid _start(void){ sys_exit(tbl[0]+tbl[3]); }\n' > $T.align.c
build $T.align.o $T.align.c
agrees "section aligned past the load base" $T.align.o
"$LNK" -o $T.v4 $T.align.o 2>/dev/null
validates "section aligned past the load base" $T.v4

# ---------------------------------------------------------- 3. readelf and the
#                                                               layout invariants
"$LNK" -o $T.v1 t/main.o t/lib.o t/asm.o 2>/dev/null
validates "three objects" $T.v1
"$LNK" -o $T.v2 t/exit42.o 2>/dev/null
validates "text-only output (no RW segment at all)" $T.v2
"$LNK" -o $T.v3 -e page_entry t/pages.o 2>/dev/null
validates "multi-page text and data" $T.v3

# .bss must be allocated without occupying file bytes. If p_filesz equals
# p_memsz the zero-initialised globals read whatever followed .data in the file.
bss=$(readelf -lW $T.v3 | python3 -c '
import sys
for f in (l.split() for l in sys.stdin):
    if f[:1] == ["LOAD"] and "RW" in f:
        fs, ms = int(f[4], 16), int(f[5], 16)
        print("yes" if fs < ms else "no: filesz %#x memsz %#x" % (fs, ms))
')
if [ "$bss" = yes ]; then ok; else no ".bss: RW segment p_filesz < p_memsz" "got: $bss"; fi

# --------------------------------------------------------- 4. refused, by name
rejects "undefined function reported by name" "nowhere_at_all" t/undef.o
printf 'extern int missing_global;\nint *p = &missing_global;\nvoid _start(void){}\n' > $T.undefdata.c
build $T.undefdata.o $T.undefdata.c
rejects "undefined global reported by name"   "missing_global" $T.undefdata.o
rejects "duplicate definition"                "duplicate definition of 'collide'" t/exit42.o t/dup.o t/dup.o
rejects "unsupported GOT relocation named"    "R_X86_64_REX_GOTPCRELX" t/gotpc.o
rejects "unknown entry symbol"                "nosuch" -e nosuch t/exit42.o
rejects "an executable is not a relocatable object" "not a relocatable object" /bin/true
head -c 300 /dev/urandom > $T.junk.o
rejects "random bytes"                        "not an ELF file" $T.junk.o
head -c 120 t/lib.o > $T.trunc.o
# "truncated or corrupt", not "lnk:": every diagnostic goes through die(),
# which prefixes "lnk: ", so that pattern matched "cannot open", "not ELF64"
# and "out of memory" equally -- it asserted only that something was printed.
rejects "truncated object"                    "truncated or corrupt" $T.trunc.o

# The .note.GNU-stack convention: a missing note means the object is asking for
# an executable stack, and that has to be said out loud.
if "$LNK" -o $T.bin -e nostack_fn $T.nostack.o 2> $T.err &&
   grep -q "executable stack" $T.err; then ok
else no "missing .note.GNU-stack is warned about" "$(cat $T.err)"; fi

# --- archives, against ar, ld and the kernel ------------------------------
#
# The selection loop is the feature. An archive of three members where
# only two are needed, and the second is needed only because the first
# referenced it, so one pass is not enough.
if command -v ar >/dev/null 2>&1; then
	build $T.arm.o t/ar_main.c && build $T.arh.o t/ar_helper.c &&
	build $T.ard.o t/ar_deeper.c && build $T.aru.o t/ar_unused.c && {
		rm -f $T.lib.a
		if ar rcs $T.lib.a $T.arh.o $T.ard.o $T.aru.o 2>$T.err; then
			runs "an archive member and the member it calls" 22 "" \
			     $T.arm.o $T.lib.a
			agrees "archive selection" $T.arm.o $T.lib.a

			# Exactly the two needed members, and not the third.
			"$LNK" -o $T.sel.bin --trace-archive $T.arm.o $T.lib.a 2>$T.tr
			got=$(sed -n 's/^lnk: pulled .*(\(.*\))$/\1/p' $T.tr | sort | tr '\n' ' ')
			want="$(basename $T.ard.o) $(basename $T.arh.o) "
			if [ "$got" = "$want" ]; then
				ok
			else
				no "only the needed members are pulled" \
				   "pulled [$got]" "wanted [$want]"
			fi

			# The transitive one specifically. If the loop ran a
			# single pass, ar_deeper would be missing and the link
			# would fail at resolve time rather than here, but the
			# message would be about an undefined symbol and not
			# about the loop.
			if grep -q "$(basename $T.ard.o)" $T.tr; then ok
			else no "the member pulled BY a pulled member is reached" \
			        "one pass is not enough and this is the case that says so"; fi

			# And the unneeded one is really in the archive, or the
			# assertion above passes because there was nothing to
			# wrongly pull.
			if ar t $T.lib.a | grep -q "$(basename $T.aru.o)"; then ok
			else no "the archive actually contains an unneeded member" \
			        "so 'only the needed members are pulled' proves nothing"; fi
		else
			no "ar rcs failed" "$(cat $T.err)"
		fi

		# Member ORDER is what makes the fixpoint necessary, and the
		# archive above does not test it: with ar_helper before
		# ar_deeper, a single pass pulls ar_helper at i=0 and then
		# sees the new reference in time to pull ar_deeper at i=1.
		# Reversed, ar_deeper is passed over before anything wants
		# it, and only a second pass reaches it. A mutation that
		# replaced the loop with one pass survived everything until
		# this archive existed.
		rm -f $T.rev.a
		if ar rcs $T.rev.a $T.ard.o $T.arh.o $T.aru.o 2>/dev/null; then
			runs "an archive whose members are in dependency order" 22 "" \
			     $T.arm.o $T.rev.a
			"$LNK" -o $T.rv.bin --trace-archive $T.arm.o $T.rev.a 2>$T.tr4
			if grep -q "$(basename $T.ard.o)" $T.tr4; then ok
			else no "the second pass reaches a member already passed over" \
			        "$(cat $T.tr4)"; fi
		else
			no "ar rcs in reverse order failed"
		fi

		# An odd-sized member. Member data is padded to an even
		# offset, and every object gcc emits here happens to be an
		# even number of bytes, so the padding rule is never
		# exercised and a mutation dropping it survives. A
		# seven-byte text file in front of the objects makes the
		# next member start one byte later than its size implies.
		printf 'ODDSIZE' > $T.odd.txt
		rm -f $T.odd.a
		if ar rcs $T.odd.a $T.odd.txt $T.arh.o $T.ard.o 2>/dev/null; then
			runs "an archive with an odd-sized member before the objects" \
			     22 "" $T.arm.o $T.odd.a
		else
			no "ar rcs with an odd-sized member failed"
		fi

		# No symbol index. ranlib's index can be stale, so lnk reads
		# each member's own .symtab and must not need the index at
		# all. `ar rc` without `s` writes none.
		rm -f $T.noidx.a
		if ar rc $T.noidx.a $T.arh.o $T.ard.o $T.aru.o 2>/dev/null; then
			runs "an archive with no symbol index" 22 "" $T.arm.o $T.noidx.a
		else
			no "ar rc (no index) failed"
		fi

		# Long member names, which live in the // member and are
		# referenced as /offset. Anything over 15 characters.
		# Under $T so the suite's own cleanup reaches it, and
		# removed here rather than left for a `rm -f $T.*` that
		# cannot remove a directory.
		longdir=$T.longnames
		rm -rf $longdir
		mkdir -p $longdir
		cp $T.arh.o $longdir/a_very_long_member_name_helper.o
		cp $T.ard.o $longdir/a_very_long_member_name_deeper.o
		rm -f $T.long.a
		if ar rcs $T.long.a $longdir/a_very_long_member_name_helper.o \
		          $longdir/a_very_long_member_name_deeper.o 2>/dev/null; then
			runs "members with names too long for the header" 22 "" \
			     $T.arm.o $T.long.a
			"$LNK" -o $T.ln.bin --trace-archive $T.arm.o $T.long.a 2>$T.tr2
			if grep -q 'a_very_long_member_name_helper.o' $T.tr2; then ok
			else no "a long member name is reported in full" "$(cat $T.tr2)"; fi
		else
			no "ar rcs with long names failed"
		fi

		rm -rf $longdir

		# A weak undefined reference does not pull a member.
		build $T.wm.o t/ar_weak_main.c && build $T.wd.o t/ar_weak_def.c && {
			rm -f $T.weak.a
			ar rcs $T.weak.a $T.wd.o 2>/dev/null
			"$LNK" -o $T.wk.bin --trace-archive $T.wm.o $T.weak.a 2>$T.tr3
			if [ -s $T.tr3 ]; then
				no "a weak undefined reference does not pull a member" \
				   "$(cat $T.tr3)"
			else
				ok
			fi
			# ld is the reference for what that program then does.
			agrees "weak reference left unresolved" $T.wm.o $T.weak.a
		}

		# A LOCAL symbol must not satisfy a reference. One member
		# has ar_contested as a static, another has it global, and
		# the static one comes first. Pulling on a local definition
		# links a member nobody can call and stops the real
		# definition being looked for.
		build $T.alo.o t/ar_local.c && build $T.agl.o t/ar_global.c &&
		build $T.awn.o t/ar_wants.c && {
			rm -f $T.bind.a
			ar rcs $T.bind.a $T.alo.o $T.agl.o 2>/dev/null
			runs "a local definition does not satisfy a reference" 5 "" \
			     $T.awn.o $T.bind.a
			"$LNK" -o $T.bd.bin --trace-archive $T.awn.o $T.bind.a 2>$T.tr5
			if grep -q "$(basename $T.alo.o)" $T.tr5; then
				no "the member with only a local definition is not pulled" \
				   "$(cat $T.tr5)"
			else
				ok
			fi
		}

		# A long-name offset past the end of the // member. No ar
		# writes one, so the guard is unreachable with any archive
		# this suite can create and a mutation deleting it survives.
		# Constructed by hand: the first member header's name field
		# is overwritten with /9999.
		if [ -f $T.long.a ]; then
			cp $T.long.a $T.badname.a
			printf '/9999          ' | dd of=$T.badname.a bs=1 seek=8 \
				conv=notrunc status=none 2>/dev/null
			if "$LNK" -o $T.x.bin $T.arm.o $T.badname.a 2>$T.err; then
				no "a long-name offset past the table is refused" "it was accepted"
			elif grep -q 'long-name table' $T.err; then ok
			else no "a long-name offset past the table is refused" "$(cat $T.err)"; fi
		fi

		# Refusals. Three shapes, because they reach three different
		# guards and the obvious one reaches none of them.
		#
		# 40 bytes is shorter than the magic plus one header, so the
		# member loop never runs and this is caught by "an archive
		# with no members". Both the size check and the end-marker
		# check survived a mutation until the two cases below
		# existed.
		head -c 40 $T.lib.a > $T.trunc.a

		# A complete first header whose size field points past the
		# end of the file.
		head -c 78 $T.lib.a > $T.shortmem.a
		if "$LNK" -o $T.x.bin $T.arm.o $T.shortmem.a 2>$T.err; then
			no "a member reaching past the end of the file is refused" \
			   "it was accepted"
		elif grep -q 'past the end' $T.err; then ok
		else no "a member reaching past the end of the file is refused" \
		        "$(cat $T.err)"; fi

		# A header with its two-byte end marker overwritten. Every
		# other field is intact, so nothing but that check can see
		# it, and without the check the parser walks off into the
		# member data treating it as headers.
		cp $T.lib.a $T.nomark.a
		printf 'XX' | dd of=$T.nomark.a bs=1 seek=66 conv=notrunc status=none 2>/dev/null
		if "$LNK" -o $T.x.bin $T.arm.o $T.nomark.a 2>$T.err; then
			no "a header with no end marker is refused" "it was accepted"
		elif grep -q 'end marker' $T.err; then ok
		else no "a header with no end marker is refused" "$(cat $T.err)"; fi

		if "$LNK" -o $T.x.bin $T.arm.o $T.trunc.a 2>$T.err; then
			no "a truncated archive is refused" "it was accepted"
		elif [ -s $T.err ]; then ok
		else no "a truncated archive is refused" "it failed without saying why"; fi

		if "$LNK" -o $T.x.bin $T.lib.a 2>$T.err; then
			no "an archive with no object to pull for is refused" "it was accepted"
		elif grep -q 'nothing to pull members for' $T.err; then ok
		else no "an archive with no object to pull for is refused" "$(cat $T.err)"; fi
	}
else
	printf 'SKIP  ar is not installed, so archives were not tested\n'
fi

printf '%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" = 0 ]
