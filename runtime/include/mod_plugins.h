#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*PSXModVBlankCallback)(void);

/*
 * Register a trusted, statically linked plugin implementation. Package
 * manifests select implementations by this stable id; archives never provide
 * native code or symbol names.
 */
int psx_mod_register_vblank_plugin(const char* id,
                                   PSXModVBlankCallback callback);

/* Narrow guest services available to trusted plugin callbacks. */
int psx_mod_game_started(void);
uint8_t psx_mod_read_byte(uint32_t address);
void psx_mod_write_byte(uint32_t address, uint8_t value);

/*
 * Register a C plugin before main() on the compilers supported by the runtime.
 * The registry itself uses function-local initialization, so constructor order
 * between game sources and the framework is safe.
 */
#if defined(_MSC_VER)
#pragma section(".CRT$XCU", read)
#define PSX_MOD_CONSTRUCTOR(name)                                           \
    static void __cdecl name(void);                                        \
    __declspec(allocate(".CRT$XCU"))                                       \
    static void (__cdecl* name##_constructor)(void) = name;                \
    static void __cdecl name(void)
#elif defined(__GNUC__) || defined(__clang__)
#define PSX_MOD_CONSTRUCTOR(name)                                           \
    static void name(void) __attribute__((constructor));                    \
    static void name(void)
#else
#error "PSX mod plugin registration needs a supported constructor mechanism"
#endif

#ifdef __cplusplus
}
#endif
