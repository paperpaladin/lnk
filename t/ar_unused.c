/* Nothing references either of these. If the selection loop pulls this
 * member, the output grows code nobody asked for, and with libc.a that
 * is the difference between a working linker and an unusable one. */
int ar_never_called(void) { return 999; }
int ar_also_never(void) { return 1000; }
