#define UNICODE
#define _UNICODE
#include <windows.h>
#include <wchar.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR arguments, int show) {
    (void)instance; (void)previous; (void)show;
    wchar_t root[32768], python[32768], command[32768];
    DWORD length = GetModuleFileNameW(NULL, root, 32768);
    if (!length || length >= 32768) return 1;
    wchar_t* end = wcsrchr(root, L'\\');
    if (!end) return 1;
    *end = 0;
    if (_snwprintf(python, 32768, L"%ls\\runtime\\pythonw.exe", root) < 0 ||
        _snwprintf(command, 32768, L"\"%ls\" -B \"%ls\\studio.py\" %ls", python, root, arguments) < 0) return 1;
    STARTUPINFOW startup = {0};
    PROCESS_INFORMATION process = {0};
    startup.cb = sizeof(startup);
    if (!CreateProcessW(python, command, NULL, NULL, FALSE, 0, NULL, root, &startup, &process)) {
        MessageBoxW(NULL, L"The Studio runtime could not start. Extract the complete application folder and try again.",
                    L"Workbench Studio", MB_OK | MB_ICONERROR);
        return 1;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return 0;
}
