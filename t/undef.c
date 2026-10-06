// Must be reported by name. A linker that quietly resolves this to zero
// produces a binary that jumps to address 0 and dies with no explanation.
int nowhere_at_all(int);
void _start(void) { nowhere_at_all(3); }
