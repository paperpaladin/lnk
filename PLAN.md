# lnk: plan

## Contract

`lnk -o OUT [-e SYM] in1.o in2.o ...`, a subset of the `ld(1)` interface. The
real contract is the **ELF64 / System V x86-64 object format and the Linux
program loader**: read what `gcc -c -fno-pie` produces, and write something
`execve` accepts. There is no partial credit in either direction: an object
file is either parsed as the assembler meant it, or it is not, and a resulting
executable either runs or the kernel kills it.

Scope today is static, non-PIE, freestanding: no dynamic linker, no libc.

## Hard invariant

Every executable lnk emits either runs correctly or was never written.

A linker has an unusually cheap way to be useless: emit a plausible wrong
address. The file links, `readelf` is happy, and then the program dies on the
first instruction with no diagnostic anywhere. So there is nothing in here that
guesses:

- An unimplemented relocation type is a hard error naming the type and the
  symbol, never a zero or a best effort.
- An undefined symbol is a hard error naming the symbol, never resolved to 0.
- Two strong definitions of one name is a hard error naming both files, never a
  silent first-wins.
- A relocation whose result does not fit its field is a hard error, never a
  truncation.
- `check_segment()` asserts, immediately before the file is written, that every
  loadable segment satisfies `p_vaddr == p_offset (mod PAGE)` and
  `p_filesz <= p_memsz`. Both mistakes produce a binary that links cleanly and
  segfaults instantly. One of them actually happened (see commit 15), which is
  why the check exists, not a comment saying to be careful.

## Oracle

Ranked by how little of it I control:

1. The kernel executes the output. `t/run.sh` links programs from multiple
   objects and runs them, asserting exit status and stdout. The programs are
   freestanding `_start` with raw syscalls, so a failure cannot be blamed on
   glibc's startup path.
2. The system `ld` links the same objects. Same inputs, two linkers, compare
   exit status and stdout. `ld` is the reference implementation and it does not
   care about my reasoning.
3. `readelf -h`, `-l`, `-S` accept the output with nothing on stderr and no
   `Error`/`Warning` in the text. It read the over-aligned-section bug out of the
   file before the kernel got a chance to.
4. The two layout invariants asserted directly out of `readelf -lW`, by
   parsing the headers back out of the file just written: segment congruence, and
   `p_filesz < p_memsz` for the segment containing `.bss`.
5. Refusal is tested as a feature. Undefined symbol by name, duplicate
   definition, unsupported relocation type by name, missing entry symbol,
   non-relocatable input, random bytes, truncated object.
6. `make test-asan` runs the entire suite under `-fsanitize=address,undefined`.
   The inputs are untrusted files. The parser is the attack surface.

   The mutation fuzzer that used to be listed here is not an oracle this repo
   has: `make test-asan` is `LNK=./lnk-asan ./t/run.sh` and nothing else, and
   there is no fuzzer in the tree (`grep -rn 'mutat\|fuzz'` finds two prose
   lines). It was run out of tree over 5,500 objects, which the README says;
   that result cannot be reproduced from here and is not part of the suite.

Oracle 3 and 1 together caught the one bug in this project that mattered. Oracle
6 caught a zero-length `memcpy` from `NULL`.

## Non-goals

Dynamic linking, `.so` output, PIE, a PLT or GOT. Linker scripts.
`--gc-sections`. `SHF_MERGE` string deduplication. `.eh_frame` rewriting or
`.eh_frame_hdr`. Symbol versioning. LTO. Any architecture but x86-64, any OS but
Linux. Incremental linking. Debug-info relocation (`.debug_*` is dropped, so
`-g` output links but is not debuggable).

## Commit sequence

Each entry is one commit: the smallest change that still builds, runs, and moves
the oracle, with the test that proves it. The relocation types get one commit
each, because that is the finest honest granularity this project has, each one is
a formula, a range check, and a program that could not be linked before it.

- [x] 01 ELF64 declarations and a bounds-checked reader for relocatable objects
      why: every later pass indexes into tables whose offsets come out of a file
           somebody else wrote, so the bounds checks have to exist before there
           is anything to bound
      files: lnk.c Makefile .gitignore LICENSE
- [x] 02 merge same-named sections, honouring each input's alignment
      why: alignment is per *input* section, not per output section; ignore that
           and the first SSE load in the second object faults
- [x] 03 the global symbol table, with duplicate definitions refused by name
      why: silently taking the first of two definitions produces a program that
           runs the wrong function, which is indistinguishable from a compiler
           bug from the outside
- [x] 04 section layout, two load segments, and the page-congruence rule
      why: p_vaddr == p_offset (mod PAGE) has no diagnostic anywhere in the
           system when it is wrong; the process just dies
- [x] 05 write the ELF header, program headers and a real section header table
      why: first commit that produces a file. Refuses any object with
           relocations instead of writing one whose every reference is wrong.
- [x] 06 the relocation dispatch, refusing every type by name
      why: the default case is the most important branch in the file, so it
           lands before any type that would make it look optional
- [x] 07 R_X86_64_PC32, and the first binary the kernel runs
      why: S + A - P, and a single-object freestanding program now links
      proves: `make test`: 1 case, the kernel executing the output
- [x] 08 R_X86_64_PLT32, so a call across object files links
      why: in a static non-PIE link there is no PLT, so this is PC32; keeping the
           smallest possible cross-object call as its own case isolates a call
           from a data reference when something later breaks
      proves: 3 cases
- [x] 09 R_X86_64_32 and R_X86_64_32S, for cross-object data references
      why: same arithmetic, different range check; getting the signedness
           backwards accepts an address the processor then sign-extends into the
           wrong half of the address space
      proves: 7 cases, including local/weak/strong resolution in both link orders
- [x] 10 R_X86_64_64
      why: the only relocation in the suite that lands in a data section, which
           is what every statically initialised function pointer is made of.
           Needs t/asm.s, because a -fno-pie C compiler never asks for it.
- [x] 11 `-e`, to name an entry point other than `_start`
      why: a missing entry symbol must be an error, never a zero e_entry
      proves: 8 cases, including the first multi-page text and data
- [x] 12 t/run.sh: ld as the reference implementation
      why: up to here every expectation was a number I chose. ld has no shared
           misunderstanding with my test suite available to it.
      proves: 13 cases
- [x] 13 t/run.sh: readelf as a validator, and the layout invariants read back
      out of the written file
      why: an invariant checked against the variables that produced it tests
           intent; checked against the bytes on disk it tests the result
      proves: 17 cases
- [x] 14 t/run.sh: refusal tested as a feature
      why: writing these found that the undefined-symbol test asserted two names
           in one run and got whichever relocation came first. The test was
           wrong, not the linker.
      proves: 27 cases
- [x] 15 fix segment layout for sections aligned past the load base
      why: the first bug that produced a binary the kernel refused to run.
           `off` and `vaddr` were aligned independently, which keeps them in step
           only for alignments that divide BASE, and the read-write segment
           header was captured before the alignment ran. readelf said "the
           segment's file size is larger than its memory size"; the kernel said
           SIGSEGV where ld's build of the same object exited 5. The parent
           commit plus this commit's t/run.sh is the red state.
      proves: 29 cases, and check_segment() makes the class unemittable
- [x] 16 `make test-asan`, and a zero-length memcpy from NULL that ubsan found
      why: the inputs are untrusted files, so the parser is the attack surface
- [x] 17 docs: README, with the honest "what does not work" list
- [x] 18 docs: PLAN, with this sequence and what comes next

### Milestone 1: link against glibc

The ordering is forced: each item below is a prerequisite for `gcc -c foo.c`
plus `libc.a` producing a program that runs. Nothing here is speculative: these
are exactly what `crt1.o` and `libc.a` turned out to need when tried.

- [ ] 04 archive (`.a`) member extraction: the `!<arch>\n` format, the `/` and
      `//` symbol-index and long-name members, and the iterate-to-fixpoint pull
      of only those members that satisfy a still-undefined symbol
      why: `libc.a` is 5,000 members. Linking all of them is not an option, so
           the selection loop is the feature, not the parser.
- [ ] 05 a GOT, and `R_X86_64_GOTPCREL`, `GOTPCRELX`, `REX_GOTPCRELX`,
      `CODE_4_GOTPCRELX`
      why: `crt1.o` as shipped uses two of these for `main` and `__libc_start_main`.
           Nothing from glibc links without them. In a static link they can also
           be *relaxed* back into a direct `lea`, which is what `ld` does and what
           avoids needing a GOT at all for most of them.
- [ ] 06 `.init_array`/`.fini_array`/`.preinit_array` collection, and the
      `__init_array_start`/`end` linker-defined symbols
      why: glibc's startup walks these, and a missing `__init_array_end` is an
           undefined-symbol error, not a mystery
- [ ] 07 the rest of the linker-defined symbols: `_end`, `_edata`, `__bss_start`,
      `__executable_start`
- [ ] 08 TLS: a `PT_TLS` segment, `.tdata`/`.tbss` layout, and `R_X86_64_TPOFF32`
      why: `errno` is thread-local, so this is reached by the first `write()`
           that fails, not by anything exotic
- [ ] 09 `R_X86_64_IRELATIVE` and `.rela.iplt`, with the static-binary
      self-relocation loop glibc's `_dl_relocate_static_pie`/`ARCH_SETUP_IREL`
      expects
      why: `memcpy` and friends are ifuncs; this is the last thing between a
           static glibc link and a running `printf`
- [ ] 10 `printf("hello\n")` from a `gcc -c` object plus `libc.a`, running
      proves: the headline claim this README currently refuses to make

### Milestone 2: dynamic linking

- [ ] 11 `PT_DYNAMIC`, `.dynsym`, `.dynstr`, `.hash`/`.gnu.hash`, and reading a
      `.so`'s dynamic symbol table instead of its `.symtab`
- [ ] 12 a real PLT and GOT with `R_X86_64_JUMP_SLOT` and `R_X86_64_GLOB_DAT`,
      plus `PT_INTERP` and `DT_NEEDED`, so `ld.so` takes over at run time
- [ ] 13 PIE: `ET_DYN` output, base 0, and `R_X86_64_RELATIVE` for every absolute
      address in the image
      why: every distribution defaults to PIE now, so a linker that cannot emit
           one is a linker you cannot use on real input

### Milestone 3: the actual point

- [ ] 14 `--gc-sections`, so `-ffunction-sections` input stops dragging in
      everything it references transitively
- [ ] 15 link `../cc`'s output and drop the `gcc` dependency. `../cc`
      currently shells out to `gcc` to assemble and link; once it emits objects
      directly and `lnk` links them, the pair needs neither gcc nor binutils to
      build a working program, and `cc` can be built by `cc` + `lnk` alone.
      why: this is what the two projects are for. Until it lands, both of them
           are still standing on the toolchain they are trying to replace.
- [ ] 16 keep binutils in the suite anyway, as the reference. The moment `ld` is
      no longer *needed*, it becomes purely an oracle, which is the better job
      for it.

## Open questions

- Output sections are ordered by class (code, read-only, writable, NOBITS) and
  then by first appearance. `ld` uses a linker script with a canonical order.
  Nothing in `t/` can tell the difference, but a program that cares about the
  relative order of two `.rodata`-ish sections would.
- The RX segment is `R|X`, so `.rodata` is mapped executable. `ld -z separate-code`
  gives read-only data its own segment. Fixing it means a third `PT_LOAD` and is
  worth doing when there is a fourth reason to touch layout.
- An over-aligned section in the *middle* of a segment is paid for in file
  padding, because `p_vaddr - p_offset` is fixed per segment and changing it
  requires a new segment. A 2MB-aligned `.rodata` produces a 2MB file. Correct,
  and absurd. Marked `shortcut:`, not fixed, since no real object does it.
- No `.symtab` in the output, so `nm` and `gdb` see nothing. Cheap to add and not
  yet needed by anything. The sanitizers and `ld` comparison have been enough.
