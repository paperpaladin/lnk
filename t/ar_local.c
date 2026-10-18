/* A member whose only symbol of that name is LOCAL. It must not be
 * pulled to satisfy a reference from elsewhere: a local definition is
 * invisible outside its own object, so pulling this member links code
 * nobody can call and, worse, may satisfy the search and stop the real
 * definition from being found. */
__attribute__((used)) static int ar_contested(void) { return 1; }
int ar_local_only_marker(void) { return ar_contested(); }
