static int shared_name(void) { return 2; }
int from_weak2(void) { return shared_name(); }
__attribute__((weak)) int strong_pick(void) { return 500; }
