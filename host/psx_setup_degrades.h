/* Bounded, read-only access to the CLI's last-recorded fallback list. */
#ifndef PSX_SETUP_DEGRADES_H
#define PSX_SETUP_DEGRADES_H
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char code[96];
    char reason[1024];
} PsxSetupDegrade;

typedef struct {
    const char* state;
    char recorded_at[48];
    size_t count;
    PsxSetupDegrade rows[128];
} PsxSetupDegradeReport;

static int psx_setup_degrade_line(FILE* file, char* line, size_t capacity) {
    size_t n;
    if (!fgets(line, (int)capacity, file)) return 0;
    n = strlen(line);
    if (!n || line[n - 1] != '\n') return 0;
    line[--n] = '\0';
    return 1;
}

static int psx_setup_degrade_text(const unsigned char* text) {
    size_t i = 0, length = strlen((const char*)text);
    while (i < length) {
        unsigned char c = text[i];
        size_t extra, j;
        if (c < 128) {
            if (c < 32 || c == 127) return 0;
            ++i;
            continue;
        }
        if (c >= 0xc2 && c <= 0xdf) extra = 1;
        else if (c >= 0xe0 && c <= 0xef) extra = 2;
        else if (c >= 0xf0 && c <= 0xf4) extra = 3;
        else return 0;
        if (i + extra >= length) return 0;
        for (j = 1; j <= extra; ++j)
            if ((text[i + j] & 0xc0) != 0x80) return 0;
        if ((c == 0xe0 && text[i + 1] < 0xa0) ||
            (c == 0xed && text[i + 1] >= 0xa0) ||
            (c == 0xf0 && text[i + 1] < 0x90) ||
            (c == 0xf4 && text[i + 1] >= 0x90)) return 0;
        i += extra + 1;
    }
    return 1;
}

static void psx_setup_degrades_read(const char* path, const char* operation,
                                   PsxSetupDegradeReport* report) {
    char line[1200], *end;
    unsigned long count;
    size_t i;
    FILE* file;
    memset(report, 0, sizeof(*report));
    errno = 0;
    file = fopen(path, "rb");
    if (!file) {
        report->state = errno == ENOENT ? "missing" : "unreadable";
        return;
    }
    report->state = "invalid";
    if (!psx_setup_degrade_line(file, line, sizeof(line)) ||
        strcmp(line, "psxrecomp-setup-degrades-v1")) goto done;
    if (!psx_setup_degrade_line(file, line, sizeof(line)) || strcmp(line, operation)) goto done;
    if (!psx_setup_degrade_line(file, line, sizeof(line))) goto done;
    if (!strcmp(line, "complete")) report->state = "recorded_complete";
    else if (!strcmp(line, "incomplete")) report->state = "recorded_incomplete";
    else goto invalid;
    if (!psx_setup_degrade_line(file, report->recorded_at, sizeof(report->recorded_at)) ||
        !report->recorded_at[0]) goto invalid;
    for (i = 0; report->recorded_at[i]; ++i) {
        char c = report->recorded_at[i];
        if (!(c >= '0' && c <= '9') && c != '-' && c != ':' && c != '.' &&
            c != '+' && c != 'T' && c != 'Z') goto invalid;
    }
    if (!psx_setup_degrade_line(file, line, sizeof(line)) || !line[0]) goto invalid;
    if (line[0] < '0' || line[0] > '9') goto invalid;
    count = strtoul(line, &end, 10);
    if (*end || count > 128) goto invalid;
    for (i = 0; i < count; ++i) {
        size_t j, code_length;
        char* tab;
        if (!psx_setup_degrade_line(file, line, sizeof(line))) goto invalid;
        tab = strchr(line, '\t');
        if (!tab || !tab[1]) goto invalid;
        code_length = (size_t)(tab - line);
        if (!code_length || code_length >= sizeof(report->rows[i].code) ||
            strlen(tab + 1) >= sizeof(report->rows[i].reason)) goto invalid;
        for (j = 0; j < code_length; ++j) {
            char c = line[j];
            if (!(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9') &&
                c != '.' && c != '_') goto invalid;
        }
        if (!psx_setup_degrade_text((const unsigned char*)(tab + 1))) goto invalid;
        *tab = '\0';
        strcpy(report->rows[i].code, line);
        strcpy(report->rows[i].reason, tab + 1);
    }
    if (fgetc(file) != EOF || ferror(file)) goto invalid;
    report->count = (size_t)count;
    goto done;
invalid:
    report->state = "invalid";
    report->recorded_at[0] = '\0';
done:
    fclose(file);
}
#endif
