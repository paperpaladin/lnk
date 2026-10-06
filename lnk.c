// lnk -- a static ELF linker for x86-64 Linux.
//
// Reads ELF64 relocatable objects (`gcc -c -fno-pie`), merges their sections,
// resolves symbols across them, applies relocations, and writes a static
// non-PIE executable that the kernel will run. No libelf, no libbfd: every
// byte of the output is written here.
//
// Deliberate corners, each marked `shortcut:` at its site:
//   - linear search over the global symbol table
//   - the whole output is built in one in-memory image, then written once
//   - no .symtab/.strtab in the output, so nm/gdb see nothing
//   - no SHF_MERGE string dedup, no section GC, no archives, no PIE, no PLT/GOT
//
// Three things here decide whether the output runs or segfaults instantly, so
// each gets a comment where it happens: the PC-relative formula S + A - P, the
// vaddr == offset (mod PAGE) congruence requirement, and p_filesz < p_memsz
// for .bss.

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// ---------------------------------------------------------------- ELF64 types
// Declared here rather than taken from <elf.h>: writing the format out is the
// project, and a self-contained source file is one fewer system header for
// ../cc to have to understand when it eventually compiles this.

typedef struct {
  unsigned char e_ident[16];
  uint16_t e_type, e_machine;
  uint32_t e_version;
  uint64_t e_entry, e_phoff, e_shoff;
  uint32_t e_flags;
  uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} Ehdr;

typedef struct {
  uint32_t sh_name, sh_type;
  uint64_t sh_flags, sh_addr, sh_offset, sh_size;
  uint32_t sh_link, sh_info;
  uint64_t sh_addralign, sh_entsize;
} Shdr;

typedef struct {
  uint32_t p_type, p_flags;
  uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} Phdr;

typedef struct {
  uint32_t st_name;
  unsigned char st_info, st_other;
  uint16_t st_shndx;
  uint64_t st_value, st_size;
} Sym;

typedef struct {
  uint64_t r_offset, r_info;
  int64_t r_addend;
} Rela;

// The on-disk layout is the struct layout only because every field here is
// naturally aligned to its own size. Assert it instead of trusting it.
_Static_assert(sizeof(Ehdr) == 64, "Ehdr");
_Static_assert(sizeof(Shdr) == 64, "Shdr");
_Static_assert(sizeof(Phdr) == 56, "Phdr");
_Static_assert(sizeof(Sym) == 24, "Sym");
_Static_assert(sizeof(Rela) == 24, "Rela");

enum { ET_REL = 1, ET_EXEC = 2 };
enum { EM_X86_64 = 62 };
enum { SHT_PROGBITS = 1, SHT_SYMTAB = 2, SHT_STRTAB = 3, SHT_RELA = 4,
       SHT_NOBITS = 8, SHT_REL = 9 };
enum { SHF_WRITE = 1, SHF_ALLOC = 2, SHF_EXECINSTR = 4 };
enum { SHN_UNDEF = 0, SHN_ABS = 0xfff1, SHN_COMMON = 0xfff2, SHN_LORESERVE = 0xff00 };
enum { STB_LOCAL = 0, STB_GLOBAL = 1, STB_WEAK = 2 };
enum { PT_LOAD = 1, PT_GNU_STACK = 0x6474e551 };
enum { PF_X = 1, PF_W = 2, PF_R = 4 };

#define ST_BIND(i) ((i) >> 4)
#define ST_TYPE(i) ((i) & 0xf)
#define R_SYM(i) ((uint32_t)((i) >> 32))
#define R_TYPE(i) ((uint32_t)((i) & 0xffffffffu))

// x86-64 relocation types. We implement the first five; everything else is
// named here only so the error message can say what it was refusing.
enum {
  R_X86_64_NONE = 0, R_X86_64_64 = 1, R_X86_64_PC32 = 2, R_X86_64_GOT32 = 3,
  R_X86_64_PLT32 = 4, R_X86_64_COPY = 5, R_X86_64_GLOB_DAT = 6,
  R_X86_64_JUMP_SLOT = 7, R_X86_64_RELATIVE = 8, R_X86_64_GOTPCREL = 9,
  R_X86_64_32 = 10, R_X86_64_32S = 11, R_X86_64_16 = 12, R_X86_64_PC16 = 13,
  R_X86_64_8 = 14, R_X86_64_PC8 = 15, R_X86_64_DTPMOD64 = 16,
  R_X86_64_DTPOFF64 = 17, R_X86_64_TPOFF64 = 18, R_X86_64_TLSGD = 19,
  R_X86_64_TLSLD = 20, R_X86_64_DTPOFF32 = 21, R_X86_64_GOTTPOFF = 22,
  R_X86_64_TPOFF32 = 23, R_X86_64_PC64 = 24, R_X86_64_GOTOFF64 = 25,
  R_X86_64_GOTPC32 = 26, R_X86_64_GOTPCRELX = 41, R_X86_64_REX_GOTPCRELX = 42,
  R_X86_64_CODE_4_GOTPCRELX = 43
};

// ---------------------------------------------------------------- diagnostics

static void die(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fputs("lnk: ", stderr);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
  exit(1);
}

// shortcut: nothing in lnk is ever freed. A linker is a short-lived process and
// exit() reclaims everything; a free() pass would be pure risk for no gain.
static void *xmalloc(size_t n) {
  void *p = calloc(1, n ? n : 1);
  if (!p) die("out of memory");
  return p;
}

// ------------------------------------------------------------------- the model

#define PAGE 0x1000ull
#define BASE 0x400000ull  // classic x86-64 non-PIE load address

// An output section: all input sections sharing a name, concatenated.
typedef struct {
  const char *name;
  uint32_t type;        // SHT_PROGBITS or SHT_NOBITS
  uint64_t flags;       // SHF_* from the inputs (they must agree)
  uint64_t align, size; // accumulated
  uint64_t addr, off;   // assigned by layout()
  int cls;              // 0 code, 1 rodata, 2 data, 3 bss -- segment ordering
} Osec;

typedef struct Obj Obj;
struct Obj {
  const char *path;
  unsigned char *buf;
  uint64_t len;
  Shdr *sh;             // detached copy, so unaligned input can't trip us
  int nsh;
  const char *shstr;
  uint64_t shstrsz;
  Sym *sym;             // detached copy of .symtab
  int nsym;
  const char *str;      // .strtab
  uint64_t strsz;
  int *osec;            // per input section: index into out[], or -1
  uint64_t *ooff;       // per input section: offset within that output section
  int gnu_stack;        // saw a .note.GNU-stack section
};

static Osec out[64];
static int nout;

static uint64_t align_up(uint64_t v, uint64_t a) {
  return a < 2 ? v : (v + a - 1) & ~(a - 1);
}

// --------------------------------------------------------------- input parsing
// Everything below treats the object file as hostile: every offset is checked
// against the file length before it is dereferenced.

static const void *at(Obj *o, uint64_t off, uint64_t n) {
  if (off > o->len || n > o->len - off)
    die("%s: truncated or corrupt: %llu bytes at offset %llu is past end of a "
        "%llu byte file",
        o->path, (unsigned long long)n, (unsigned long long)off,
        (unsigned long long)o->len);
  return o->buf + off;
}

static void *dup_at(Obj *o, uint64_t off, uint64_t n) {
  const void *p = at(o, off, n);
  void *q = xmalloc(n ? n : 1);
  memcpy(q, p, n);
  return q;
}

// A name out of a string table, checked so a corrupt st_name cannot walk off
// the end of the buffer.
static const char *strat(Obj *o, const char *tab, uint64_t tabsz, uint64_t i,
                         const char *what) {
  if (i >= tabsz) die("%s: %s name offset %llu is outside its string table",
                      o->path, what, (unsigned long long)i);
  if (!memchr(tab + i, 0, tabsz - i))
    die("%s: unterminated %s name", o->path, what);
  return tab + i;
}

static const char *secname(Obj *o, int i) {
  return strat(o, o->shstr, o->shstrsz, o->sh[i].sh_name, "section");
}

static void read_obj(Obj *o, const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) die("cannot open %s", path);
  if (fseek(f, 0, SEEK_END)) die("cannot seek %s", path);
  long n = ftell(f);
  if (n < 0) die("cannot size %s", path);
  rewind(f);
  o->path = path;
  o->len = (uint64_t)n;
  o->buf = xmalloc(o->len ? o->len : 1);
  if (o->len && fread(o->buf, 1, o->len, f) != o->len)
    die("short read on %s", path);
  fclose(f);

  if (o->len < sizeof(Ehdr)) die("%s: too short to be an ELF file", path);
  Ehdr eh;
  memcpy(&eh, o->buf, sizeof eh);
  if (memcmp(eh.e_ident, "\177ELF", 4)) die("%s: not an ELF file", path);
  if (eh.e_ident[4] != 2) die("%s: not ELF64 (we only link 64-bit objects)", path);
  if (eh.e_ident[5] != 1) die("%s: not little-endian", path);
  if (eh.e_type != ET_REL)
    die("%s: not a relocatable object (e_type=%u); lnk links .o files, not "
        "executables or shared libraries",
        path, eh.e_type);
  if (eh.e_machine != EM_X86_64) die("%s: not x86-64 (e_machine=%u)", path, eh.e_machine);
  if (eh.e_shentsize != sizeof(Shdr))
    die("%s: e_shentsize is %u, expected 64", path, eh.e_shentsize);
  if (eh.e_shnum == 0 || eh.e_shnum >= SHN_LORESERVE)
    die("%s: e_shnum=%u; lnk does not implement the SHN_XINDEX escape for "
        "objects with 0xff00 or more sections",
        path, eh.e_shnum);

  o->nsh = eh.e_shnum;
  o->sh = dup_at(o, eh.e_shoff, (uint64_t)o->nsh * sizeof(Shdr));

  if (eh.e_shstrndx >= (uint16_t)o->nsh) die("%s: bad e_shstrndx", path);
  Shdr *ss = &o->sh[eh.e_shstrndx];
  o->shstrsz = ss->sh_size;
  o->shstr = dup_at(o, ss->sh_offset, ss->sh_size);

  // .symtab plus the .strtab it points at. An object with no symbol table is
  // legal but useless to us, so it just contributes bytes.
  for (int i = 0; i < o->nsh; i++) {
    if (o->sh[i].sh_type != SHT_SYMTAB) continue;
    if (o->sym) die("%s: more than one .symtab", path);
    if (o->sh[i].sh_entsize != sizeof(Sym))
      die("%s: .symtab entsize is %llu, expected 24", path,
          (unsigned long long)o->sh[i].sh_entsize);
    if (o->sh[i].sh_size % sizeof(Sym)) die("%s: ragged .symtab", path);
    o->nsym = (int)(o->sh[i].sh_size / sizeof(Sym));
    o->sym = dup_at(o, o->sh[i].sh_offset, o->sh[i].sh_size);
    uint32_t li = o->sh[i].sh_link;
    if (li >= (uint32_t)o->nsh || o->sh[li].sh_type != SHT_STRTAB)
      die("%s: .symtab sh_link does not name a string table", path);
    o->strsz = o->sh[li].sh_size;
    o->str = dup_at(o, o->sh[li].sh_offset, o->sh[li].sh_size);
  }

  o->osec = xmalloc((size_t)o->nsh * sizeof(int));
  o->ooff = xmalloc((size_t)o->nsh * sizeof(uint64_t));
  for (int i = 0; i < o->nsh; i++) o->osec[i] = -1;

  for (int i = 1; i < o->nsh; i++) {
    if (!strcmp(secname(o, i), ".note.GNU-stack")) o->gnu_stack = 1;
    if (o->sh[i].sh_type == SHT_REL)
      die("%s: SHT_REL section '%s'; x86-64 uses SHT_RELA and lnk only reads "
          "RELA",
          path, secname(o, i));
    // Section contents must actually be in the file, except NOBITS which by
    // definition is not.
    if (o->sh[i].sh_type != SHT_NOBITS) at(o, o->sh[i].sh_offset, o->sh[i].sh_size);
  }
}

// ----------------------------------------------------------- section gathering

static int seccls(uint64_t flags, uint32_t type) {
  if (!(flags & SHF_WRITE)) return (flags & SHF_EXECINSTR) ? 0 : 1;
  return type == SHT_NOBITS ? 3 : 2;
}

static void gather(Obj *objs, int nobj) {
  for (int oi = 0; oi < nobj; oi++) {
    Obj *o = &objs[oi];
    for (int i = 1; i < o->nsh; i++) {
      Shdr *s = &o->sh[i];
      if (!(s->sh_flags & SHF_ALLOC)) continue;  // .comment, debug info, notes
      // gcc emits an empty .data and .bss in almost every object. Dropping them
      // here keeps the output from growing a zero-length section and, when every
      // input is like that, a zero-length PT_LOAD.
      if (s->sh_size == 0) continue;
      if (s->sh_type != SHT_PROGBITS && s->sh_type != SHT_NOBITS)
        die("%s: allocated section '%s' has type %u, which lnk cannot place",
            o->path, secname(o, i), s->sh_type);
      const char *nm = secname(o, i);
      Osec *os = NULL;
      for (int k = 0; k < nout; k++)
        if (!strcmp(out[k].name, nm)) { os = &out[k]; break; }
      if (!os) {
        if (nout == (int)(sizeof out / sizeof *out))
          die("too many distinct output sections (limit %d)", nout);
        os = &out[nout++];
        os->name = nm;
        os->type = s->sh_type;
        os->flags = s->sh_flags;
        os->align = 1;
        os->cls = seccls(s->sh_flags, s->sh_type);
      }
      // A NOBITS input merged into a PROGBITS output would need file bytes we
      // are not going to write, and vice versa. gcc never does it; say so
      // rather than produce a silently short section.
      if (os->type != s->sh_type)
        die("%s: section '%s' is type %u here but %u in an earlier object",
            o->path, nm, s->sh_type, os->type);
      if (s->sh_addralign > os->align) os->align = s->sh_addralign;
      o->osec[i] = (int)(os - out);
      o->ooff[i] = align_up(os->size, s->sh_addralign ? s->sh_addralign : 1);
      os->size = o->ooff[i] + s->sh_size;
    }
  }

  // Stable sort by class so the read-only sections form one contiguous run and
  // the writable ones another: that is what lets two PT_LOAD segments cover
  // everything, with .bss last so it can be the part with no file bytes.
  for (int i = 1; i < nout; i++)
    for (int j = i; j > 0 && out[j - 1].cls > out[j].cls; j--) {
      Osec t = out[j - 1];
      out[j - 1] = out[j];
      out[j] = t;
      for (int oi = 0; oi < nobj; oi++)
        for (int k = 1; k < objs[oi].nsh; k++) {
          if (objs[oi].osec[k] == j - 1) objs[oi].osec[k] = j;
          else if (objs[oi].osec[k] == j) objs[oi].osec[k] = j - 1;
        }
    }
}

// -------------------------------------------------------------------- main

int main(int argc, char **argv) {
  const char *outpath = "a.out";
  const char *in[256];
  int nin = 0;

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-o")) {
      if (++i == argc) die("-o needs an argument");
      outpath = argv[i];
    } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
      fputs("usage: lnk -o OUT in1.o in2.o ...\n", stdout);
      return 0;
    } else if (argv[i][0] == '-' && argv[i][1]) {
      die("unknown option '%s' (lnk takes -o)", argv[i]);
    } else {
      if (nin == 256) die("too many input files");
      in[nin++] = argv[i];
    }
  }
  if (!nin) die("no input files");
  (void)outpath;

  Obj *objs = xmalloc((size_t)nin * sizeof(Obj));
  for (int i = 0; i < nin; i++) read_obj(&objs[i], in[i]);

  gather(objs, nin);
  if (!nout) die("no allocatable sections in the input: nothing to link");

  // No output yet, so print the merged section table. It is the only way to see
  // whether the merge and the alignment are right before addresses exist.
  for (int i = 0; i < nout; i++)
    printf("%-18s size %8llu  align %4llu\n", out[i].name,
           (unsigned long long)out[i].size, (unsigned long long)out[i].align);
  return 0;
}
