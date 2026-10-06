// The other half of the multi-object test. Nothing here calls anything local:
// every reference crosses the object boundary, which is the point.
int counter = 5;                       // .data, read from main.o
const char msg[] = "hello from lib\n"; // .rodata, read from main.o
int bigbuf[4096];                      // .bss, written by main.o

int add(int a, int b) { return a + b; }

long wr(int fd, const void *p, long n); // defined in main.o: a call back out

int lib_sum(void) {
  int s = 0;
  for (int i = 0; i < 4096; i++) s += bigbuf[i];
  return s;
}

// Proves .bss really is zero-filled at run time rather than whatever followed
// .data in the file image.
int bss_is_zero(void) {
  for (int i = 0; i < 4096; i++)
    if (i != 7 && bigbuf[i] != 0) return 0;
  return 1;
}

int say(void) { return (int)wr(1, msg, sizeof msg - 1); }
