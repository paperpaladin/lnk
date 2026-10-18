int ar_contested(void);
void _start(void) {
  long r = ar_contested();
  __asm__ volatile("mov $60, %%eax\n\tsyscall" :: "D"(r) : "rax");
  __builtin_unreachable();
}
