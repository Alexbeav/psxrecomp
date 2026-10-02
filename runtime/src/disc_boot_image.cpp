/* disc_boot_image.cpp — see disc_boot_image.h. */
#include "disc_boot_image.h"

#include "iso_reader.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

uint8_t *psx_read_disc_boot_image(const std::string &exe_path,
                                  const std::string &disc_path,
                                  uint32_t *out_len,
                                  std::string *out_name) {
    if (!disc_path.empty()) {
        PS1::ISOReader iso;
        if (iso.Open(disc_path)) {
            /* The boot filename: basename of the game.toml exe field (it names
             * the same file the recompiler consumed, which came off this disc);
             * fall back to the SYSTEM.CNF BOOT token for configs whose local
             * name differs from the disc name. */
            std::string boot_name;
            if (!exe_path.empty()) {
                const size_t slash = exe_path.find_last_of("/\\");
                boot_name = (slash == std::string::npos)
                                ? exe_path : exe_path.substr(slash + 1);
            }
            PS1::ISOFileEntry ent;
            if (boot_name.empty() || !iso.FindFile(boot_name, ent)) {
                /* SYSTEM.CNF: `BOOT = cdrom:\SCUS_944.23;1` */
                uint8_t cnf[2048] = {0};
                size_t n = iso.ReadFile("SYSTEM.CNF", cnf, sizeof(cnf) - 1);
                if (n > 0) {
                    std::string text((const char *)cnf, n);
                    std::string lower = text;
                    for (char &c : lower) c = (char)std::tolower((unsigned char)c);
                    const size_t key = lower.find("cdrom:");
                    if (key != std::string::npos) {
                        size_t j = key + 6;
                        while (j < text.size() && (text[j] == '\\' || text[j] == '/')) j++;
                        std::string tok;
                        while (j < text.size()) {
                            char c = text[j];
                            if (c == ';' || c == '\r' || c == '\n' || c == ' ' ||
                                c == '\t' || c == '\0') break;
                            tok += c; j++;
                            if (tok.size() > 64) break;
                        }
                        const size_t s2 = tok.find_last_of("\\/");
                        if (s2 != std::string::npos) tok = tok.substr(s2 + 1);
                        if (!tok.empty() && iso.FindFile(tok, ent)) boot_name = tok;
                    }
                }
            }
            if (!boot_name.empty() && iso.FindFile(boot_name, ent) &&
                ent.size > 2048) {
                uint8_t *file = (uint8_t *)std::malloc(ent.size);
                if (file) {
                    size_t got = iso.ReadFile(boot_name, file, ent.size);
                    if (got > 2048) {
                        uint32_t img_len = (uint32_t)(got - 2048);
                        uint8_t *img = (uint8_t *)std::malloc(img_len);
                        if (img) {
                            memcpy(img, file + 2048, img_len);
                            std::free(file);
                            *out_len = img_len;
                            *out_name = boot_name;
                            return img;
                        }
                    }
                    std::free(file);
                }
            }
        }
    }
    return nullptr;
}
