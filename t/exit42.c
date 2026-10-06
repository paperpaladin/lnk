// A freestanding program: no libc, no crt1.o, no dynamic loader. The kernel
// jumps straight at _start and the only way out is the exit_group syscall.
// Starting here means a linker bug cannot be mistaken for a glibc startup bug.
static void sys_exit(long code) {
  __asm__ volatile("syscall" :: "a"(60L), "D"(code) : "memory");
  __builtin_unreachable();
}

void _start(void) { sys_exit(42); }
