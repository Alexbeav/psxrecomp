#include "debug_server.h"
#include <stdlib.h>
#include <string.h>

static int requested;

void debug_server_request(int enabled) { requested = enabled != 0; }

int debug_server_requested(void)
{
    const char *env = getenv("PSX_DEBUG_SERVER");
    if (env && strcmp(env, "0") == 0) return 0;
    if (env && strcmp(env, "1") == 0) return 1;
#ifdef PSX_NO_DEBUG_TOOLS
    return requested;
#else
    return 1;
#endif
}

int debug_server_resolve_port(int configured, const char *env, int cli)
{
    if (cli >= 0 && cli <= 65535) return cli;
    if (env && *env) {
        char *end = NULL;
        long v = strtol(env, &end, 10);
        if (end != env && *end == '\0' && v >= 0 && v <= 65535) return (int)v;
    }
    return configured;
}

void debug_server_apply_request(int port)
{
    int listening = 0;
    debug_server_get_status(&listening, NULL, NULL);
    if (debug_server_requested()) {
        if (!listening) debug_server_init(port);
    } else if (listening) {
        debug_server_shutdown();
    }
}
