#include "debug_server.h"
#include <stdio.h>
#include <stdlib.h>

static int listening, starts, stops, last_port, failures;
void debug_server_get_status(int *active, int *port, int *error) {
    if (active) *active=listening;
    if (port) *port=last_port;
    if (error) *error=0;
}
void debug_server_init(int port) { ++starts;listening=1;last_port=port; }
void debug_server_shutdown(void) { ++stops;listening=0; }
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x);++failures; } } while(0)
static void env_choice(const char *value) {
#ifdef _WIN32
    _putenv_s("PSX_DEBUG_SERVER",value?value:"");
#else
    if(value)setenv("PSX_DEBUG_SERVER",value,1);else unsetenv("PSX_DEBUG_SERVER");
#endif
}
int main(void) {
    env_choice(NULL);
    debug_server_request(0);debug_server_apply_request(4370);
    CHECK(!listening && starts==0 && stops==0);
    /* First session was vanilla netplay; the next offline session selects it. */
    debug_server_request(1);debug_server_apply_request(4370);
    CHECK(listening && starts==1 && last_port==4370);
    debug_server_apply_request(4370);
    CHECK(listening && starts==1 && stops==0);
    debug_server_request(0);debug_server_apply_request(4370);
    CHECK(!listening && stops==1);
    debug_server_apply_request(4370);CHECK(stops==1);
    debug_server_request(1);env_choice("0");debug_server_apply_request(4370);
    CHECK(!listening && starts==1);
    debug_server_request(0);env_choice("1");debug_server_apply_request(4488);
    CHECK(listening && starts==2 && last_port==4488);
    env_choice(NULL);debug_server_apply_request(4488);
    CHECK(!listening && stops==2);
    /* PS1B-194: --debug-port beats PSX_DEBUG_PORT beats the configured port;
     * 0 (OS-assigned) is a real choice at every level; junk env is ignored. */
    CHECK(debug_server_resolve_port(4370, NULL, -1) == 4370);
    CHECK(debug_server_resolve_port(4370, "", -1) == 4370);
    CHECK(debug_server_resolve_port(4370, "4555", -1) == 4555);
    CHECK(debug_server_resolve_port(4370, "0", -1) == 0);
    CHECK(debug_server_resolve_port(4370, "4555", 4600) == 4600);
    CHECK(debug_server_resolve_port(4370, "4555", 0) == 0);
    CHECK(debug_server_resolve_port(4370, "45x", -1) == 4370);
    CHECK(debug_server_resolve_port(4370, "70000", -1) == 4370);
    CHECK(debug_server_resolve_port(4370, "-1", -1) == 4370);
    if(!failures)puts("PASS debug server session policy: late enable, reuse, disable, overrides, port precedence");
    return failures?1:0;
}
