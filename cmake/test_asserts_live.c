/* PS1B-590: fails when a test executable is built with NDEBUG defined, which
 * turns every assert() in it into nothing. psx_keep_test_asserts() removes it. */
#include <stdio.h>

int main(void) {
#ifdef NDEBUG
    fputs("NDEBUG is defined: assert() is compiled out of this test\n", stderr);
    return 1;
#else
    return 0;
#endif
}
