/* jflaunch: minimal Lyra plugin that starts a normal user-mode program.
 * Keeps our real code OUT of nativeapp's kernel-privileged process, so a crash
 * there kills only that program instead of rebooting the Zune.
 * arg: "<exe path>|<command line>"   out: the new process id, or the error.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int run(const void *arg, int arg_len, void *out, int out_max, int *out_used) {
    char a[600], *bar; wchar_t exe[260], cmd[400];
    PROCESS_INFORMATION pi; int len, n;
    len = arg_len < (int)sizeof a - 1 ? arg_len : (int)sizeof a - 1;
    if (len < 0) len = 0;
    memcpy(a, arg, len); a[len] = 0;
    bar = strchr(a, '|');
    if (bar) *bar++ = 0; else bar = a + len;
    MultiByteToWideChar(CP_UTF8, 0, a, -1, exe, 260);
    MultiByteToWideChar(CP_UTF8, 0, bar, -1, cmd, 400);
    memset(&pi, 0, sizeof pi);
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, NULL, &pi)) {
        n = _snprintf((char *)out, out_max, "CreateProcess(%s) failed, err %lu\r\n", a, GetLastError());
    } else {
        n = _snprintf((char *)out, out_max, "started %s, pid %lu\r\n", a, pi.dwProcessId);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }
    if (out_used) *out_used = n < 0 ? 0 : n;
    return 0;
}

__declspec(dllexport) int Run(const void *arg, int arg_len, void *out, int out_max, int *out_used)
{ return run(arg, arg_len, out, out_max, out_used); }

BOOL WINAPI DllMain(HANDLE h, DWORD r, LPVOID l) { return TRUE; }
