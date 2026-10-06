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

static const char *reloc_name(uint32_t t) {
  switch (t) {
  case R_X86_64_NONE: return "R_X86_64_NONE";
  case R_X86_64_64: return "R_X86_64_64";
  case R_X86_64_PC32: return "R_X86_64_PC32";
  case R_X86_64_GOT32: return "R_X86_64_GOT32";
  case R_X86_64_PLT32: return "R_X86_64_PLT32";
  case R_X86_64_GOTPCREL: return "R_X86_64_GOTPCREL";
  case R_X86_64_32: return "R_X86_64_32";
  case R_X86_64_32S: return "R_X86_64_32S";
  case R_X86_64_16: return "R_X86_64_16";
  case R_X86_64_PC16: return "R_X86_64_PC16";
  case R_X86_64_8: return "R_X86_64_8";
  case R_X86_64_PC8: return "R_X86_64_PC8";
  case R_X86_64_TLSGD: return "R_X86_64_TLSGD";
  case R_X86_64_TLSLD: return "R_X86_64_TLSLD";
  case R_X86_64_DTPOFF32: return "R_X86_64_DTPOFF32";
  case R_X86_64_GOTTPOFF: return "R_X86_64_GOTTPOFF";
  case R_X86_64_TPOFF32: return "R_X86_64_TPOFF32";
  case R_X86_64_PC64: return "R_X86_64_PC64";
  case R_X86_64_GOTOFF64: return "R_X86_64_GOTOFF64";
  case R_X86_64_GOTPC32: return "R_X86_64_GOTPC32";
  case R_X86_64_GOTPCRELX: return "R_X86_64_GOTPCRELX";
  case R_X86_64_REX_GOTPCRELX: return "R_X86_64_REX_GOTPCRELX";
  case R_X86_64_CODE_4_GOTPCRELX: return "R_X86_64_CODE_4_GOTPCRELX";
  default: return "unknown";
  }
}

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

static void warn(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fputs("lnk: warning: ", stderr);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
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

// A resolved definition. shortcut: linear search, O(refs * defs). Fine for the
// handful of objects a static link of a freestanding program involves; swap in
// a hash table when linking glibc's archive members makes it matter.
typedef struct {
  const char *name;
  Obj *o;
  uint32_t idx;  // index into o->sym
  int weak;
} Gsym;

static Gsym *gsym;
static int ngsym, cgsym;

static Gsym *gfind(const char *name) {
  for (int i = 0; i < ngsym; i++)
    if (!strcmp(gsym[i].name, name)) return &gsym[i];
  return NULL;
}

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

static const char *symname(Obj *o, uint32_t i) {
  if ((int)i >= o->nsym) die("%s: symbol index %u out of range", o->path, i);
  if (ST_TYPE(o->sym[i].st_info) == 3 /* STT_SECTION */ &&
      o->sym[i].st_name == 0) {
    uint16_t sx = o->sym[i].st_shndx;
    if (sx < (uint16_t)o->nsh) return secname(o, sx);
  }
  return strat(o, o->str, o->strsz, o->sym[i].st_name, "symbol");
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

// ----------------------------------------------------------------- the layout

static uint64_t rx_end, rw_off, rw_file_end, rw_vend, img_end;
static int have_rw;
static uint64_t hdrsz;
static int nph;

static void layout(void) {
  nph = 2;  // one LOAD, plus PT_GNU_STACK
  for (int i = 0; i < nout; i++)
    if (out[i].flags & SHF_WRITE) { nph = 3; have_rw = 1; break; }
  hdrsz = sizeof(Ehdr) + (uint64_t)nph * sizeof(Phdr);

  // The first segment starts at file offset 0 so the headers themselves are
  // mapped, which is what ld does and what makes vaddr == BASE + offset hold for
  // everything in it.
  uint64_t off = hdrsz, vaddr = BASE + hdrsz;
  rx_end = hdrsz;

  for (int i = 0; i < nout; i++) {
    Osec *os = &out[i];
    if ((os->flags & SHF_WRITE) && !rw_off) {
      // A new segment. Writable data cannot share a page with the read-execute
      // text, because mmap's granularity is a page and a page gets one set of
      // permissions. So round the file offset up to a page.
      rx_end = off;
      off = align_up(off, PAGE);
      vaddr = BASE + off;
      rw_off = off;
    }
    // Every loadable segment must satisfy p_vaddr == p_offset (mod PAGE). The
    // kernel maps whole pages: it mmaps from p_offset rounded down to a page
    // boundary at p_vaddr rounded down likewise, so if the low 12 bits of the
    // two disagree, every byte in the segment lands at the wrong address and the
    // first instruction fetched is garbage. Keeping vaddr == BASE + off, with
    // BASE page-aligned, makes the congruence hold by construction instead of by
    // arithmetic we could get wrong.
    off = align_up(off, os->align);
    vaddr = align_up(vaddr, os->align);
    os->off = off;
    os->addr = vaddr;
    vaddr += os->size;
    if (os->type != SHT_NOBITS) off += os->size;
  }
  if (!rw_off) rx_end = off;
  rw_file_end = off;
  rw_vend = vaddr;
  img_end = off;
}

// ------------------------------------------------------- symbols and addresses

static uint64_t secaddr(Obj *o, uint16_t shndx, const char *what) {
  if (shndx >= (uint16_t)o->nsh)
    die("%s: %s lives in section %u, which does not exist", o->path, what, shndx);
  if (o->osec[shndx] < 0)
    die("%s: %s lives in section '%s', which lnk did not place in the output "
        "(it is either not SHF_ALLOC or empty)",
        o->path, what, secname(o, shndx));
  return out[o->osec[shndx]].addr + o->ooff[shndx];
}

static uint64_t gval(Gsym *g) {
  Sym *s = &g->o->sym[g->idx];
  if (s->st_shndx == SHN_ABS) return s->st_value;
  return secaddr(g->o, s->st_shndx, g->name) + s->st_value;
}

// Register every global definition, diagnosing duplicates. Weak loses to
// strong; two strong definitions of the same name is an error, because
// silently picking one is how a linker produces a program that runs the wrong
// code.
static void resolve_defs(Obj *objs, int nobj) {
  for (int oi = 0; oi < nobj; oi++) {
    Obj *o = &objs[oi];
    for (int i = 0; i < o->nsym; i++) {
      Sym *s = &o->sym[i];
      int bind = ST_BIND(s->st_info);
      if (bind == STB_LOCAL) continue;
      if (s->st_shndx == SHN_UNDEF) continue;
      if (s->st_shndx == SHN_COMMON)
        die("%s: '%s' is a COMMON symbol; lnk does not allocate commons. "
            "Compile with -fno-common (gcc's default since 10) so tentative "
            "definitions land in .bss.",
            o->path, symname(o, (uint32_t)i));
      const char *nm = symname(o, (uint32_t)i);
      int weak = bind == STB_WEAK;
      Gsym *g = gfind(nm);
      if (g) {
        if (!weak && !g->weak)
          die("duplicate definition of '%s': in %s and %s", nm, g->o->path, o->path);
        if (!weak) { g->o = o; g->idx = (uint32_t)i; g->weak = 0; }
        continue;
      }
      if (ngsym == cgsym) {
        cgsym = cgsym ? cgsym * 2 : 64;
        Gsym *n = xmalloc((size_t)cgsym * sizeof(Gsym));
        memcpy(n, gsym, (size_t)ngsym * sizeof(Gsym));
        gsym = n;
      }
      gsym[ngsym].name = nm;
      gsym[ngsym].o = o;
      gsym[ngsym].idx = (uint32_t)i;
      gsym[ngsym].weak = weak;
      ngsym++;
    }
  }
}

// The value S of the symbol a relocation names.
static uint64_t symval(Obj *o, uint32_t i, const char *ctx) {
  if ((int)i >= o->nsym) die("%s: relocation names symbol %u, past end of .symtab", o->path, i);
  Sym *s = &o->sym[i];
  const char *nm = symname(o, i);
  if (ST_BIND(s->st_info) != STB_LOCAL) {
    Gsym *g = gfind(nm);
    if (g) return gval(g);  // the winning definition, which may be elsewhere
  }
  if (s->st_shndx == SHN_UNDEF) {
    // An undefined weak reference is defined by the ABI to be address zero.
    if (ST_BIND(s->st_info) == STB_WEAK) return 0;
    die("undefined symbol '%s', referenced by %s (%s)", nm, o->path, ctx);
  }
  if (s->st_shndx == SHN_ABS) return s->st_value;
  if (s->st_shndx == SHN_COMMON)
    die("%s: reference to COMMON symbol '%s'; recompile with -fno-common", o->path, nm);
  return secaddr(o, s->st_shndx, nm) + s->st_value;
}

// ------------------------------------------------------------- the output image
// shortcut: the entire output is built in one in-memory buffer and written with
// a single fwrite, rather than seeking around a stream. The buffer is the size
// of the program being linked, so the ceiling is linking something larger than
// RAM; the upgrade is pwrite per section.

static unsigned char *img;
static uint64_t imgsz;

static void put16(uint64_t off, uint16_t v) { memcpy(img + off, &v, 2); }
static void put32(uint64_t off, uint32_t v) { memcpy(img + off, &v, 4); }
static void put64(uint64_t off, uint64_t v) { memcpy(img + off, &v, 8); }

static void copy_sections(Obj *objs, int nobj) {
  for (int oi = 0; oi < nobj; oi++) {
    Obj *o = &objs[oi];
    for (int i = 1; i < o->nsh; i++) {
      if (o->osec[i] < 0) continue;
      Shdr *s = &o->sh[i];
      if (s->sh_type == SHT_NOBITS) continue;  // .bss: nothing to copy
      memcpy(img + out[o->osec[i]].off + o->ooff[i], at(o, s->sh_offset, s->sh_size),
             s->sh_size);
    }
  }
}

static void apply_rela(Obj *o, int ri) {
  Shdr *rs = &o->sh[ri];
  if (rs->sh_entsize != sizeof(Rela)) die("%s: RELA entsize is not 24", o->path);
  if (rs->sh_size % sizeof(Rela)) die("%s: ragged RELA section", o->path);
  uint32_t ti = rs->sh_info;
  if (ti >= (uint32_t)o->nsh) die("%s: RELA sh_info out of range", o->path);
  if (o->osec[ti] < 0) return;  // relocations for a section we did not keep
  Osec *os = &out[o->osec[ti]];
  if (os->type == SHT_NOBITS)
    die("%s: relocations against '%s', which has no file contents", o->path,
        secname(o, (int)ti));
  uint64_t base_off = os->off + o->ooff[ti];
  uint64_t base_addr = os->addr + o->ooff[ti];
  uint64_t seclen = o->sh[ti].sh_size;
  const char *where = secname(o, (int)ti);

  uint64_t n = rs->sh_size / sizeof(Rela);
  const unsigned char *raw = at(o, rs->sh_offset, rs->sh_size);
  for (uint64_t k = 0; k < n; k++) {
    Rela r;
    memcpy(&r, raw + k * sizeof(Rela), sizeof r);
    uint32_t type = R_TYPE(r.r_info);
    if (type == R_X86_64_NONE) continue;
    uint64_t width = (type == R_X86_64_64 || type == R_X86_64_PC64) ? 8 : 4;
    if (r.r_offset > seclen || width > seclen - r.r_offset)
      die("%s: relocation at offset %llu runs past the end of '%s'", o->path,
          (unsigned long long)r.r_offset, where);

    uint64_t S = symval(o, R_SYM(r.r_info), where);
    int64_t A = r.r_addend;
    // P is the address of the relocated field *in the output*, which is only
    // knowable after layout -- hence the ordering of the passes above.
    uint64_t P = base_addr + r.r_offset;
    uint64_t at_off = base_off + r.r_offset;
    const char *nm = symname(o, R_SYM(r.r_info));

    switch (type) {
    case R_X86_64_64:
      put64(at_off, S + (uint64_t)A);
      break;
    case R_X86_64_PC32:
    case R_X86_64_PLT32: {
      // S + A - P. A call or RIP-relative access encodes the distance from the
      // *end* of the instruction to the target; the assembler puts that -4 (or
      // whatever the trailing-byte count is) into the addend, so the linker
      // just subtracts the address of the field itself. Get the sign of this
      // backwards and every call jumps to roughly twice the wrong distance.
      //
      // PLT32 is identical to PC32 here: in a static non-PIE link there is no
      // PLT, so a call that the compiler was willing to route through one just
      // becomes a direct relative branch to the final address.
      int64_t v = (int64_t)(S + (uint64_t)A - P);
      if (v < INT32_MIN || v > INT32_MAX)
        die("%s: %s against '%s' does not fit in 32 bits (displacement %lld); "
            "the output image is larger than a 2GB PC-relative branch can reach",
            o->path, reloc_name(type), nm, (long long)v);
      put32(at_off, (uint32_t)(int32_t)v);
      break;
    }
    case R_X86_64_32: {
      uint64_t v = S + (uint64_t)A;
      if (v > 0xffffffffull)
        die("%s: R_X86_64_32 against '%s' resolves to 0x%llx, which does not "
            "fit in an unsigned 32-bit field",
            o->path, nm, (unsigned long long)v);
      put32(at_off, (uint32_t)v);
      break;
    }
    case R_X86_64_32S: {
      int64_t v = (int64_t)(S + (uint64_t)A);
      if (v < INT32_MIN || v > INT32_MAX)
        die("%s: R_X86_64_32S against '%s' resolves to 0x%llx, which does not "
            "fit in a signed 32-bit field",
            o->path, nm, (unsigned long long)v);
      put32(at_off, (uint32_t)(int32_t)v);
      break;
    }
    default:
      // Refusing beats emitting a plausible wrong address: a binary that links
      // cleanly and then jumps into nowhere costs far more to debug.
      die("%s: unsupported relocation type %u (%s) against '%s' in '%s'.\n"
          "       lnk implements R_X86_64_64, PC32, PLT32, 32 and 32S only. "
          "GOT/PLT/TLS forms need a dynamic linker's machinery (see PLAN.md); "
          "refusing rather than emitting a wrong address.",
          o->path, type, reloc_name(type), nm, where);
    }
  }
}

// ------------------------------------------------------------------- the write

// Sizes of everything past the last segment, so the image can be allocated
// before section contents are copied into it.
static uint64_t shstr_off, shstrsz, shoff;
static int nsh;

// shortcut: no .symtab or .strtab in the output, so nm and gdb see nothing in a
// linked binary (objdump -d still works, since the section headers are real).
// Copying the input symbol tables out with their values fixed up is maybe 40
// lines; nothing needs it yet.
static void alloc_image(void) {
  shstr_off = img_end;
  shstrsz = 1;
  for (int i = 0; i < nout; i++) shstrsz += strlen(out[i].name) + 1;
  shstrsz += strlen(".shstrtab") + 1;
  shoff = align_up(shstr_off + shstrsz, 8);
  nsh = nout + 2;  // SHN_UNDEF, ours, .shstrtab
  imgsz = shoff + (uint64_t)nsh * sizeof(Shdr);
  img = xmalloc(imgsz);
}

static void write_out(const char *path, uint64_t entry, int exec_stack) {
  put32(0, 0x464c457fu);        // \x7fELF
  img[4] = 2;                   // ELFCLASS64
  img[5] = 1;                   // ELFDATA2LSB
  img[6] = 1;                   // EV_CURRENT
  img[7] = 0;                   // ELFOSABI_NONE
  put16(16, ET_EXEC);
  put16(18, EM_X86_64);
  put32(20, 1);
  put64(24, entry);
  put64(32, sizeof(Ehdr));      // e_phoff
  put64(40, shoff);
  put32(48, 0);                 // e_flags
  put16(52, sizeof(Ehdr));
  put16(54, sizeof(Phdr));
  put16(56, (uint16_t)nph);
  put16(58, sizeof(Shdr));
  put16(60, (uint16_t)nsh);
  put16(62, (uint16_t)(nsh - 1));  // e_shstrndx

  uint64_t p = sizeof(Ehdr);
  // RX: the headers plus everything not writable.
  put32(p, PT_LOAD);
  put32(p + 4, PF_R | PF_X);
  put64(p + 8, 0);
  put64(p + 16, BASE);
  put64(p + 24, BASE);
  put64(p + 32, rx_end);
  put64(p + 40, rx_end);
  put64(p + 48, PAGE);
  p += sizeof(Phdr);

  if (have_rw) {
    put32(p, PT_LOAD);
    put32(p + 4, PF_R | PF_W);
    put64(p + 8, rw_off);
    put64(p + 16, BASE + rw_off);
    put64(p + 24, BASE + rw_off);
    // .bss is the whole reason these two numbers differ. It is allocated at run
    // time but occupies no bytes in the file, so p_filesz counts only the
    // initialised data and p_memsz extends past it; the kernel zero-fills the
    // difference. Make them equal and every global that should start at zero
    // instead reads whatever bytes follow .data in the file -- or faults,
    // because the page holding it was never mapped at all.
    put64(p + 32, rw_file_end - rw_off);
    put64(p + 40, rw_vend - (BASE + rw_off));
    put64(p + 48, PAGE);
    p += sizeof(Phdr);
  }

  // The .note.GNU-stack convention: an object carrying that (empty, unallocated)
  // section is declaring that it does not need an executable stack. The
  // linker's job is to turn the unanimous agreement of all the inputs into a
  // PT_GNU_STACK without PF_X, which is the thing the kernel actually reads.
  put32(p, PT_GNU_STACK);
  put32(p + 4, exec_stack ? (uint32_t)(PF_R | PF_W | PF_X) : (uint32_t)(PF_R | PF_W));
  put64(p + 48, 0x10);

  // Section headers. Entry 0 is the all-zero SHN_UNDEF entry, already zero.
  uint64_t so = shoff + sizeof(Shdr);
  uint64_t nameoff = 1;
  for (int i = 0; i < nout; i++, so += sizeof(Shdr)) {
    size_t l = strlen(out[i].name) + 1;
    memcpy(img + shstr_off + nameoff, out[i].name, l);
    put32(so, (uint32_t)nameoff);
    nameoff += l;
    put32(so + 4, out[i].type);
    put64(so + 8, out[i].flags);
    put64(so + 16, out[i].addr);
    put64(so + 24, out[i].off);
    put64(so + 32, out[i].size);
    put64(so + 48, out[i].align);
  }
  memcpy(img + shstr_off + nameoff, ".shstrtab", 10);
  put32(so, (uint32_t)nameoff);
  put32(so + 4, SHT_STRTAB);
  put64(so + 24, shstr_off);
  put64(so + 32, shstrsz);
  put64(so + 48, 1);

  FILE *f = fopen(path, "wb");
  if (!f) die("cannot create %s", path);
  if (fwrite(img, 1, imgsz, f) != imgsz) die("short write on %s", path);
  if (fclose(f)) die("cannot close %s", path);
  // The kernel will not exec a file without the x bit, and nothing else here is
  // going to set it.
  if (chmod(path, 0755)) die("cannot make %s executable", path);
}

int main(int argc, char **argv) {
  const char *outpath = "a.out", *entry = "_start";
  const char *in[256];
  int nin = 0;

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-o")) {
      if (++i == argc) die("-o needs an argument");
      outpath = argv[i];
    } else if (!strcmp(argv[i], "-e")) {
      if (++i == argc) die("-e needs an argument");
      entry = argv[i];
    } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
      fputs("usage: lnk -o OUT [-e SYM] in1.o in2.o ...\n", stdout);
      return 0;
    } else if (argv[i][0] == '-' && argv[i][1]) {
      die("unknown option '%s' (lnk takes -o and -e)", argv[i]);
    } else {
      if (nin == 256) die("too many input files");
      in[nin++] = argv[i];
    }
  }
  if (!nin) die("no input files");

  Obj *objs = xmalloc((size_t)nin * sizeof(Obj));
  for (int i = 0; i < nin; i++) read_obj(&objs[i], in[i]);

  gather(objs, nin);
  if (!nout) die("no allocatable sections in the input: nothing to link");
  layout();
  resolve_defs(objs, nin);

  Gsym *e = gfind(entry);
  if (!e)
    die("entry symbol '%s' is not defined in any input (use -e to name a "
        "different one)",
        entry);
  uint64_t entry_addr = gval(e);

  int exec_stack = 0;
  for (int i = 0; i < nin; i++)
    if (!objs[i].gnu_stack) exec_stack = 1;
  if (exec_stack)
    warn("at least one input has no .note.GNU-stack section, so the output "
         "requests an executable stack");

  alloc_image();
  copy_sections(objs, nin);
  for (int oi = 0; oi < nin; oi++)
    for (int i = 1; i < objs[oi].nsh; i++)
      if (objs[oi].sh[i].sh_type == SHT_RELA) apply_rela(&objs[oi], i);

  write_out(outpath, entry_addr, exec_stack);
  return 0;
}
