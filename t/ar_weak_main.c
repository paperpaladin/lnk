/* A weak undefined reference must NOT pull a member. That is what weak
 * means: if nothing supplies it the symbol is zero and the link
 * succeeds. A linker that treats it as a normal reference links code
 * nobody asked for. */
__attribute__((weak)) int ar_weakly_wanted(void);
void _start(void) {
  long r = ar_weakly_wanted ? 1 : 7;
  __asm__ volatile("mov $60, %%eax\n\tsyscall" :: "D"(r) : "rax");
  __builtin_unreachable();
}
