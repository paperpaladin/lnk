// Entry object. Talks to the kernel directly and pulls everything else in from
// lib.o and asm.o, so every interesting reference is cross-object.
extern int counter;
extern const char msg[];
extern int bigbuf[];
int add(int a, int b);
int lib_sum(void);
int bss_is_zero(void);
int say(void);
int asm_probe(void);

long wr(int fd, const void *p, long n) {
  long r;
  __asm__ volatile("syscall" : "=a"(r) : "a"(1L), "D"(fd), "S"(p), "d"(n)
                   : "rcx", "r11", "memory");
  return r;
}

static void sys_exit(long code) {
  __asm__ volatile("syscall" :: "a"(60L), "D"(code) : "memory");
  __builtin_unreachable();
}

static void putnum(int v) {
  char b[16];
  int i = 16;
  if (v == 0) b[--i] = '0';
  while (v > 0) { b[--i] = (char)('0' + v % 10); v /= 10; }
  b[15] = b[15];
  wr(1, b + i, 16 - i);
  wr(1, "\n", 1);
}

void _start(void) {
  say();                 // lib.o calls back into wr() here
  if (!bss_is_zero()) { wr(2, "bss dirty\n", 10); sys_exit(1); }
  bigbuf[7] = 26;        // cross-object .bss write
  int r = add(counter, 2);   // cross-object call reading cross-object .data: 7
  r += lib_sum();            // 26
  r += asm_probe();          // 9
  putnum(r);
  sys_exit(r);
}
