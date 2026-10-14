# lnk

A static ELF linker for x86-64 Linux. Reads `gcc -c` relocatable objects, merges
their sections, resolves symbols across them, applies relocations, and writes a
non-PIE executable that the kernel runs.

```
$ make
cc -std=c11 -g -O1 -Wall -Wextra -Wno-unused-parameter -fno-common -ffile-prefix-map=/home/x/projects/lnk=. -o lnk lnk.c

$ gcc -c -fno-pie -O1 -ffreestanding -o t/main.o t/main.c
$ gcc -c -fno-pie -O1 -ffreestanding -o t/lib.o t/lib.c
$ gcc -c -o t/asm.o t/asm.s
$ ./lnk -o prog t/main.o t/lib.o t/asm.o && ./prog; echo "\$? = $?"
hello from lib
42
$? = 42
```

```
$ make test
29 passed, 0 failed

$ make test-asan
29 passed, 0 failed
```

## Why this exists

It pairs with `../cc`. That compiler emits System V assembly and then shells out
to `gcc` to assemble and link it, which means it is still standing on the
toolchain it is trying to replace. `lnk` is the other half: once `cc` emits
objects directly and `lnk` links them, the two together need neither gcc nor
binutils to turn C into a running program. Dropping the binutils dependency is
the point, and it is item 15 in `PLAN.md`, not a thing that works today.

Writing a linker is also the shortest path to understanding why ELF has the
shape it does. Three details decide whether a hand-written linker produces a
working binary or one that dies on its first instruction, and all three are
invisible until you get one wrong:

- `S + A - P`. A `call` encodes the distance from the end of the instruction
  to its target. The assembler puts the trailing-byte correction into the
  addend, so the linker computes symbol + addend - *the address of the field it
  is writing*. Get the sign backwards and every call lands roughly twice the
  wrong distance away.
- `p_vaddr == p_offset (mod PAGE)`. The kernel maps a segment by `mmap`ing
  from `p_offset` rounded down to a page boundary, at `p_vaddr` rounded down
  likewise. If the low 12 bits disagree, every byte in the segment is loaded at
  an address that does not match where the relocations pointed.
- `p_filesz < p_memsz`. `.bss` is allocated but occupies no bytes in the
  file. The kernel zero-fills the difference between the two sizes. Make them
  equal and every zero-initialised global reads whatever happened to follow
  `.data` in the file instead, or faults, because the page was never mapped.

The second one is the bug that actually happened here. See "Status".

## The oracle

The kernel runs the output or it does not. That is the whole test suite's
spine: `t/run.sh` links programs from multiple objects and executes them,
asserting exit status and stdout. The test programs are freestanding: a
`_start` that makes raw `write` and `exit_group` syscalls, so a failure cannot
be blamed on glibc's startup path.

Two more oracles sit behind it, both outside my control:

- `ld` is the reference implementation. Six cases are linked twice, once
  with `lnk` and once with the system `ld`, and the two programs must produce
  identical stdout and identical exit statuses. Three of those six are
  multi-object. This used to say "every multi-object case", which is not so:
  nine cases take more than one object file and six of them: the
  cross-object call pairs in both orders, the three-object pair in both
  orders, and the two weak-resolution cases, go through `runs`, which checks
  lnk's own output against an expected exit status and stdout,
  not against `ld`. No hand-computed address appears anywhere in the suite either
  way.
- `readelf` is the validator. `readelf -h`, `-l` and `-S` must accept the
  output with nothing on stderr and no `Error` or `Warning` in the text. The
  harness then parses `readelf -lW` back out of the file it just wrote and
  asserts the two layout invariants above directly.

Refusal is tested as a feature, because a linker's most expensive failure mode is
succeeding wrongly: undefined symbol reported by name, duplicate definition
naming both files, unsupported relocation type named, missing entry symbol,
non-relocatable input, truncated object, random bytes.

`make test-asan` reruns the whole suite under
`-fsanitize=address,undefined`, since every byte the parser touches came out of
an untrusted file. An out-of-tree mutation fuzzer (5,500 corrupted and truncated
objects) and `valgrind` are both clean.

## Status

Freestanding programs link and run. **libc does not.** That is the honest
headline.

### What works

- Input: ELF64 `ET_REL` x86-64 objects. Section headers, `.symtab`,
  `.strtab`, `.shstrtab`, `.rela.*`. Every offset is bounds-checked against the
  file length before it is dereferenced.
- Section merge: all `SHF_ALLOC` `PROGBITS`/`NOBITS` sections, grouped by
  name, concatenated in input order with each input section's own
  `sh_addralign` honoured. `.text`, `.rodata`, `.rodata.str1.1`, `.data`,
  `.bss`, `.eh_frame` all come through generically. Nothing is special-cased by
  name. Empty sections are dropped, which is why a text-only program gets one
  segment, not a zero-length second one.
- Symbols: locals stay local (two objects can each have a different `static`
  function of the same name), globals resolve across objects in either link
  order, a strong definition beats a weak one regardless of which was seen
  first, an undefined weak reference becomes address 0 as the ABI says, and two
  strong definitions of one name is an error naming both files.
- Relocations: `R_X86_64_64`, `R_X86_64_PC32`, `R_X86_64_PLT32`,
  `R_X86_64_32`, `R_X86_64_32S`, and `R_X86_64_NONE`. `PLT32` is handled
  identically to `PC32`: in a static non-PIE link there is no PLT, so a call the
  compiler was willing to route through one becomes a direct relative branch.
  Every one of the five is exercised by the suite: `32S` and `64` need
  `t/asm.s`, because a `-fno-pie` C compiler prefers RIP-relative and never
  emits them. Each form range-checks its result and errors,
  not truncating.
- Output: `ET_EXEC` at `0x400000`, one `R|X` `PT_LOAD`, one `R|W` `PT_LOAD`
  when there is writable data, `PT_GNU_STACK` without `PF_X`, correct `e_entry`,
  and a real section header table with `.shstrtab` so `readelf -S` and `objdump`
  are useful.
- `.note.GNU-stack`: the convention is that an object carrying that empty
  section is declaring it does not need an executable stack. `lnk` emits
  `PT_GNU_STACK` with `R|W`, and if *any* input lacks the note it warns and sets
  `PF_X`, instead of quietly deciding either way.
- CLI: `lnk -o OUT [-e SYM] in1.o in2.o ...`. Default entry is `_start`.

### What does not

- libc. Not even close, and for specific reasons, not general ones.
  `crt1.o` as shipped needs `R_X86_64_GOTPCRELX` and `R_X86_64_REX_GOTPCRELX`,
  which `lnk` refuses. Past that, `libc.a` needs archive member extraction with
  the pull-to-fixpoint selection loop, `.init_array` collection, the
  `_end`/`__bss_start` linker-defined symbols, a `PT_TLS` segment with
  `R_X86_64_TPOFF32` for `errno`, and `R_X86_64_IRELATIVE` resolution for the
  ifunc `memcpy`. Those are milestone 1 in `PLAN.md`, in that order, because that
  is the order they were hit in. **Only freestanding `_start` programs link
  today.**
- Relocation types other than the six above. Including every GOT form
  (`GOT32`, `GOTPCREL`, `GOTPCRELX`, `REX_GOTPCRELX`, `CODE_4_GOTPCRELX`,
  `GOTOFF64`, `GOTPC32`), every TLS form (`TLSGD`, `TLSLD`, `DTPOFF32`,
  `GOTTPOFF`, `TPOFF32`, `TPOFF64`, `DTPMOD64`, `DTPOFF64`), the dynamic forms
  (`COPY`, `GLOB_DAT`, `JUMP_SLOT`, `RELATIVE`, `IRELATIVE`), `PC64`, and the
  narrow forms `16`/`PC16`/`8`/`PC8`. Every one of them is an error naming the
  type and the symbol. This is the single most important design decision in the
  project: a linker that guesses is worse than one that refuses.
- No dynamic linking. No `PT_DYNAMIC`, `.dynsym`, PLT, GOT, `PT_INTERP` or
  `DT_NEEDED`. Shared libraries cannot be an input, and a `.so` cannot be an
  output.
- No PIE. Output is `ET_EXEC` at a fixed `0x400000`. `-fPIC` input is
  rejected, because that is exactly what produces the GOT relocations above.
- No archives. A `.a` on the command line is rejected as not an ELF file.
- No section garbage collection, so `-ffunction-sections` input keeps
  everything. No `SHF_MERGE` string deduplication, so an identical string
  literal in two objects occupies two copies.
- No linker scripts. Section order and the load address are compiled in.
- No `.symtab` in the output, so `nm` and `gdb` see nothing in a linked
  binary. `objdump -d` works, because the section headers are real.
- `.eh_frame` is merged but not rewritten, and no `.eh_frame_hdr` /
  `PT_GNU_EH_FRAME` is produced. Harmless for freestanding code; C++ exceptions
  would not unwind.
- `SHN_COMMON` symbols are rejected with a message pointing at `-fno-common`,
  which has been gcc's default since gcc 10.
- No `SHN_XINDEX`, so an object with 65,280 or more sections is rejected.
- `.debug_*` is dropped, so `-g` input links but the result is not
  debuggable.

### The bug that mattered

Everything above worked on the first run, which was suspicious, so I went
looking for the case I had got wrong. It was section alignment larger than the
load base. `layout()` was aligning the file offset and the virtual address
independently. Since `BASE` is `0x400000`, any power-of-two alignment up to 4MB
divides it and the two stay in step by luck. An
`__attribute__((aligned(0x800000)))` array breaks that, and the segment header
was additionally computed from an offset captured *before* the alignment ran.
`readelf` caught it first, "the segment's file size is larger than its memory
size", and the kernel confirmed it with a SIGSEGV where `ld`'s build of the same
object exited 5.

The fix is not arithmetic but an invariant: `p_vaddr - p_offset` is now held
fixed per segment, and the file offset is *derived* from the virtual address,
not computed alongside it. `check_segment()` then asserts the congruence
and `p_filesz <= p_memsz` immediately before the file is written, so this entire
class of bug can no longer leave the process as a binary.

## Design

One file, 855 lines. The ELF64 structures are declared in it, not taken
from `<elf.h>`, because writing the format out is the project and because a
self-contained translation unit is one fewer system header for `../cc` to have to
understand later. `_Static_assert` pins each structure's size, since the on-disk
layout is the struct layout only by the accident that every field is naturally
aligned.

Five passes, in an order forced by the data: parse every object, assign input
sections to output sections, lay the output sections out to get addresses,
register symbol definitions, then relocate. The relocation pass has to be last
because `P`: the address of the field being written, is not knowable until
layout has run.

Object files are treated as hostile. Section and symbol table contents are
`memcpy`'d out into freshly allocated arrays, not cast in place, which
both avoids unaligned access and gives the sanitizers something to police; every
offset goes through a bounds-checked accessor. Every string table index is
checked for range and for termination.

Five deliberate corners, each marked `shortcut:` in the source with its ceiling:

- Linear search over the global symbol table. Fine for the handful of objects
  a freestanding link involves; needs a hash table before `libc.a`'s thousands of
  members.
- The whole output is built as one in-memory image and written once. Simpler
  than seeking around a stream, and the buffer is the size of the program; the
  ceiling is linking something bigger than RAM.
- No `.symtab` in the output. Copying the input symbol tables out with their
  values fixed up is about 40 lines. Nothing needs it yet.
- An over-aligned section mid-segment is paid for in file padding,
  not with a third segment.
- Nothing is freed. A linker is a short-lived process; `exit` reclaims.

## Tests

`t/run.sh` is the whole harness. Four helpers, one per oracle: `runs` asserts
exit status and stdout of the linked program, `agrees` links the same objects
with `ld` and compares, `validates` runs `readelf` and checks the layout
invariants out of the written file, and `rejects` asserts that bad input fails
with a given string in the message.

The test inputs are small and deliberate. `t/main.c` holds the entry point and
the raw syscalls; `t/lib.c` holds the data and the functions, and calls *back*
into `t/main.o`, so every interesting reference crosses an object boundary in
both directions. `t/asm.s` exists only because a C compiler will not emit
`R_X86_64_32S` or `R_X86_64_64` for a `-fno-pie` target. Two more cases are
generated by the harness because the source is not the interesting part: the
smallest possible cross-object call, which isolates a call from a data reference
so that a later failure says which of the two broke, and the multi-page case,
text past one page, two pages of initialised data, eight of `.bss`, is generated
by the harness, not checked in, because the interesting part is the page
crossing and not the source.

## License

GPL-3.0-or-later. This is a tool, not a library. I would rather a fork of
it stay open than get embedded in something closed.
