/* Calls one archive member, which calls another. The second is pulled
 * only because the first referenced it, which is what makes this a
 * test of the selection loop rather than of the parser. */
int ar_helper(int x);
void _start(void) {
  int r = ar_helper(20);
  __asm__ volatile("mov $60, %%eax\n\tsyscall" :: "D"((long)r) : "rax");
  __builtin_unreachable();
}
