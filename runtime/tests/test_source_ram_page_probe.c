/* Exercise the production diagnostic with synthetic RAM, including long movies. */
#include "source_ram_page_probe.h"
#include <signal.h>
static uint8_t test_ram[2097152];
uint8_t *memory_get_ram_ptr(void) { return test_ram; }
static void expected_abort(int ignored) { (void)ignored; exit(4); }
int main(int argc, char **argv) {
    signal(SIGABRT, expected_abort);
    if (argc != 2) return 2;
    test_ram[0] = 0x34;
    test_ram[2097151] = 0xab;
    source_ram_page_probe((unsigned)strtoul(argv[1], NULL, 10), UINT64_C(40500000000));
    return 0;
}
