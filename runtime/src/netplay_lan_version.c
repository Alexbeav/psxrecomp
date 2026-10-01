/* The build check for LAN / Direct IP rooms (PS1B-295). See netplay_lan_version.h. */
#include "netplay_lan_version.h"

#include <string.h>

void netplay_lan_join_version(const char *tail, char *out, size_t out_cap)
{
    const char *p = tail;
    size_t n = 0;
    int i;

    if (!out || !out_cap) return;
    out[0] = '\0';
    if (!p) return;
    for (i = 0; i < NETPLAY_LAN_VERSION_LINE; ++i) {
        const char *nl = strchr(p, '\n');
        if (!nl) return;
        p = nl + 1;
    }
    while (p[n] && p[n] != '\n' && p[n] != '\r' && n + 1 < out_cap) {
        out[n] = p[n];
        ++n;
    }
    out[n] = '\0';
}

/* Blank or missing means "dev", as recomp-net-server normalises it. */
static const char *normalised(const char *version, size_t *len)
{
    const char *end;

    if (!version) version = "";
    while (*version == ' ' || *version == '\t') ++version;
    end = version + strlen(version);
    while (end > version && (end[-1] == ' ' || end[-1] == '\t' ||
                             end[-1] == '\r' || end[-1] == '\n'))
        --end;
    if (end == version) {
        *len = 3;
        return "dev";
    }
    *len = (size_t)(end - version);
    return version;
}

int netplay_lan_version_ok(const char *host_version, const char *guest_version)
{
    size_t host_len, guest_len;
    const char *host = normalised(host_version, &host_len);
    const char *guest = normalised(guest_version, &guest_len);

    return host_len == guest_len && memcmp(host, guest, host_len) == 0;
}
