// Three resolution rules that a linker can get silently wrong: a local symbol
// must not be visible to other objects, a strong definition must beat a weak
// one, and an undefined weak reference must become address zero rather than an
// error.
static int shared_name(void) { return 1; }     // also in weak2.c, must not clash
int strong_pick(void) { return 10; }           // weak twin in weak2.c
__attribute__((weak)) int weak_loser(void) { return 99; }
extern int absent_weak(void) __attribute__((weak));

static void sys_exit(long c) {
  __asm__ volatile("syscall" :: "a"(60L), "D"(c) : "memory");
  __builtin_unreachable();
}
int from_weak2(void);

void _start(void) {
  int r = shared_name() + from_weak2();   // 1 + 2 = 3
  r += strong_pick();                     // 13
  if (absent_weak) r += 1000;             // undefined weak == 0, so not taken
  sys_exit(r);
}
