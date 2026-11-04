/* jfping — Zune HD network test, built as a Lyra plugin DLL with GCC (arm-mingw32ce).
 *
 * Runs inside Lyra's nativeapp daemon (which owns Winsock: no WSAStartup here).
 * arg: "<jellyfin-ip> <port>"   e.g. "192.168.1.50 8096"
 * out: the raw HTTP reply from GET /System/Info/Public, or an error line.
 * Returns 0 on success, negative on failure.
 */
#include <windows.h>
#include <winsock2.h>
#include <stdio.h>
#include <string.h>

#define EXPORT __declspec(dllexport)

static int run(const void *arg, int arg_len, void *out, int out_max, int *out_used)
{
    char host[64] = "", req[256], *o = (char *)out;
    int port = 8096, used = 0, n;

    if (out_used) *out_used = 0;
    if (arg && arg_len > 0) {
        char a[128];
        int len = arg_len < (int)sizeof a - 1 ? arg_len : (int)sizeof a - 1;
        memcpy(a, arg, len); a[len] = 0;
        sscanf(a, "%63s %d", host, &port);
    }
    if (!host[0]) {
        used = _snprintf(o, out_max, "usage: --arg \"<ip> <port>\"\r\n");
        if (out_used) *out_used = used;
        return -1;
    }

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        used = _snprintf(o, out_max, "socket() failed, err %d\r\n", WSAGetLastError());
        if (out_used) *out_used = used;
        return -2;
    }

    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    a.sin_addr.s_addr = inet_addr(host);
    if (connect(s, (struct sockaddr *)&a, sizeof a) != 0) {
        used = _snprintf(o, out_max, "connect to %s:%d failed, err %d\r\n",
                         host, port, WSAGetLastError());
        closesocket(s);
        if (out_used) *out_used = used;
        return -3;
    }

    n = _snprintf(req, sizeof req,
                  "GET /System/Info/Public HTTP/1.0\r\nHost: %s:%d\r\n"
                  "User-Agent: ZuneFin/0.0\r\n\r\n", host, port);
    send(s, req, n, 0);

    while (used < out_max - 1 && (n = recv(s, o + used, out_max - 1 - used, 0)) > 0)
        used += n;
    closesocket(s);

    if (out_used) *out_used = used;
    return used > 0 ? 0 : -4;
}

/* Lyra's loaders look for "Run" (README) or "Activate" (lyra-plugin-call.py default). */
EXPORT int Run(const void *arg, int arg_len, void *out, int out_max, int *out_used)
{ return run(arg, arg_len, out, out_max, out_used); }

EXPORT int Activate(const void *arg, int arg_len, void *out, int out_max, int *out_used)
{ return run(arg, arg_len, out, out_max, out_used); }

BOOL WINAPI DllMain(HANDLE h, DWORD reason, LPVOID r) { return TRUE; }
