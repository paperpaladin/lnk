// -fPIC routes even a local global through the GOT, which is a relocation form
// that needs machinery lnk does not have. It must say so, not guess.
int ext = 1;
int getext(void) { return ext; }
void _start(void) {}
