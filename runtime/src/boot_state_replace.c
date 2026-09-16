/* boot_state_replace.c — atomic file replace for state saves.
 *
 * Separated from boot_state.c so the overwrite/refusal behaviour can be
 * regression-tested without linking the whole serializer.
 *
 * On Windows the CRT's rename() does NOT replace an existing target, and a
 * remove()+rename() sequence is not atomic — a failure between them loses the
 * old file. MoveFileExW with MOVEFILE_REPLACE_EXISTING is the real atomic
 * replace. MOVEFILE_WRITE_THROUGH is added so the replace is not deferred.
 *
 * A failed replace must REFUSE the save (the caller returns 0) rather than
 * retry quietly: antivirus and indexers briefly locking a freshly written file
 * is a common Windows flake, and a silent retry hides it.
 */
#include "boot_state.h"

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <stdio.h>
#endif

int boot_state_replace_file(const char* from, const char* to) {
    if (!from || !to) return 0;
#if defined(_WIN32)
    {
        wchar_t wfrom[1024], wto[1024];
        if (MultiByteToWideChar(CP_UTF8, 0, from, -1, wfrom, 1024) <= 0) return 0;
        if (MultiByteToWideChar(CP_UTF8, 0, to,   -1, wto,   1024) <= 0) return 0;
        return MoveFileExW(wfrom, wto,
                           MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? 1 : 0;
    }
#else
    return rename(from, to) == 0;
#endif
}
