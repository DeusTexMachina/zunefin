/* jfplay: ZuneFin playback test #2, a Lyra plugin DLL built with GCC (arm-mingw32ce).
 *
 * Picks a random song from Jellyfin, builds a stream URL, and hands it to the Zune's
 * own player via zdksystem!ZDKMedia_Queue_PlaySongFromURL(title, url). The stock
 * firmware registers an http:// DirectShow source (zwmtstreamer), so a plain
 * HTTP stream may play with no custom decoder.
 *
 * arg: "<ip> <port> <api_key> [mode] [item_id]"
 *   mode: file (default)  download MP3 to \flash2\zunefin\now.mp3, then PlaySongFromFile
 *         mp3            stream URL: Jellyfin transcodes to MP3 192k
 *         static         the original file, untouched
 *         aac            Jellyfin transcodes to AAC in an ADTS stream
 *         url            don't play; just print the URL (sanity check)
 *   item_id: play this item instead of a random one
 * out: what happened, as text.
 */
#include <windows.h>
#include <winsock2.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

#define EXPORT __declspec(dllexport)

typedef int (*PlaySongFromUrlFn)(const wchar_t *title, const wchar_t *url);
typedef int (*PlaySongFromFileFn)(const wchar_t *title, const wchar_t *path, int duration_ms);

typedef int (*QGetActiveIdxFn)(int *idx);
typedef int (*QGetSongAtFn)(int idx, void **song);
typedef int (*ZdkSetPropFn)(void *song, DWORD prop, DWORD val);
/* Internal zdksystem setters found by Lyra's YouTube mod (firmware 4.5, XIP fixed VA). */
#define ZDK_SETPROP ((ZdkSetPropFn)0x4198474c)   /* string props: 0x20002 artist, 0x20003 album */
#define ZDK_SETINT  ((ZdkSetPropFn)0x4198464c)   /* int props: 0x10008 has-art */

#define CACHE_DIR  L"\\flash2\\zunefin"


/* Step log that survives a reboot: open/append/close every line. */
static void flog(const char *fmt, ...) {
    char line[300]; va_list ap; int n; HANDLE h; DWORD w;
    va_start(ap, fmt); n = _vsnprintf(line, sizeof line - 3, fmt, ap); va_end(ap);
    if (n < 0) n = sizeof line - 3;
    line[n++] = '\r'; line[n++] = '\n';
    h = CreateFileW(L"\\flash2\\zunefin.log", GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    SetFilePointer(h, 0, NULL, FILE_END);
    WriteFile(h, line, n, &w, NULL);
    FlushFileBuffers(h);
    CloseHandle(h);
}

static char *o_buf; static int o_max, o_used;
static void say(const char *fmt, ...) {
    va_list ap; int n;
    if (o_used >= o_max - 1) return;
    va_start(ap, fmt);
    n = _vsnprintf(o_buf + o_used, o_max - 1 - o_used, fmt, ap);
    va_end(ap);
    if (n < 0) n = o_max - 1 - o_used;
    o_used += n; o_buf[o_used] = 0;
}

/* HTTP/1.0 GET; returns malloc'd body (NUL-terminated) or NULL. */
static char *http_get(const char *host, int port, const char *path, int *status) {
    SOCKET s; struct sockaddr_in a; char req[700]; int n, cap = 65536, len = 0;
    char *buf, *body;
    *status = 0;
    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { say("socket failed %d\r\n", WSAGetLastError()); return NULL; }
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((u_short)port); a.sin_addr.s_addr = inet_addr(host);
    if (connect(s, (struct sockaddr *)&a, sizeof a)) {
        say("connect %s:%d failed %d\r\n", host, port, WSAGetLastError()); closesocket(s); return NULL;
    }
    n = _snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: %s:%d\r\nUser-Agent: ZuneFin/0.1\r\n\r\n",
                  path, host, port);
    send(s, req, n, 0);
    buf = (char *)malloc(cap + 1);
    if (!buf) { closesocket(s); return NULL; }
    while (len < cap && (n = recv(s, buf + len, cap - len, 0)) > 0) len += n;
    closesocket(s);
    buf[len] = 0;
    if (len > 12 && !strncmp(buf, "HTTP/1.", 7)) *status = atoi(buf + 9);
    body = strstr(buf, "\r\n\r\n");
    if (!body) { free(buf); return NULL; }
    memmove(buf, body + 4, strlen(body + 4) + 1);
    return buf;
}


/* Stream an HTTP/1.0 GET body straight into a file. Returns bytes written, -1 on error. */
static long http_download(const char *host, int port, const char *path, const wchar_t *dest, int *status) {
    SOCKET s; struct sockaddr_in a; char req[700], *buf; int n, hdr = 0, hlen = 0;
    long total = 0; DWORD w; HANDLE f; char *e;
    *status = 0;
    buf = (char *)malloc(8192);
    if (!buf) return -1;
    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { free(buf); return -1; }
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((u_short)port); a.sin_addr.s_addr = inet_addr(host);
    if (connect(s, (struct sockaddr *)&a, sizeof a)) { closesocket(s); free(buf); return -1; }
    n = _snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: %s:%d\r\nUser-Agent: ZuneFin/0.1\r\n\r\n",
                  path, host, port);
    send(s, req, n, 0);
    flog("dl: request sent");
    CreateDirectoryW(CACHE_DIR, NULL);
    f = CreateFileW(dest, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) { say("can't create cache file (err %lu)\r\n", GetLastError()); closesocket(s); free(buf); return -1; }
    flog("dl: cache file open");
    /* read the header into buf, then body straight to disk */
    while (!hdr && (n = recv(s, buf + hlen, 8191 - hlen, 0)) > 0) {
        hlen += n; buf[hlen] = 0;
        if ((e = strstr(buf, "\r\n\r\n")) != NULL) {
            int body = hlen - (int)(e + 4 - buf);
            hdr = 1;
            if (!strncmp(buf, "HTTP/1.", 7)) *status = atoi(buf + 9);
            if (body > 0) { WriteFile(f, e + 4, body, &w, NULL); total += body; }
        } else if (hlen >= 8191) break;
    }
    while (hdr && (n = recv(s, buf, 8192, 0)) > 0) {
        if (!WriteFile(f, buf, n, &w, NULL) || (int)w != n) { say("write failed (disk full?)\r\n"); total = -1; break; }
        total += n;
        if ((total & 0xFFFFF) < n) flog("dl: %ld KB", total / 1024);
    }
    CloseHandle(f); closesocket(s); free(buf);
    return total;
}

/* Append code point cp to out as UTF-8. */
static int put_utf8(char *out, int i, int cap, unsigned cp) {
    if (cp < 0x80) { if (i < cap - 1) out[i++] = (char)cp; }
    else if (cp < 0x800) { if (i < cap - 2) { out[i++] = (char)(0xC0 | (cp >> 6)); out[i++] = (char)(0x80 | (cp & 0x3F)); } }
    else if (cp < 0x10000) { if (i < cap - 3) { out[i++] = (char)(0xE0 | (cp >> 12)); out[i++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[i++] = (char)(0x80 | (cp & 0x3F)); } }
    else { if (i < cap - 4) { out[i++] = (char)(0xF0 | (cp >> 18)); out[i++] = (char)(0x80 | ((cp >> 12) & 0x3F)); out[i++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[i++] = (char)(0x80 | (cp & 0x3F)); } }
    return i;
}

static unsigned hex4(const char *q) {
    unsigned v = 0; int k;
    for (k = 0; k < 4; k++) {
        char c = q[k]; v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return 0xFFFD;
    }
    return v;
}

/* Decode a JSON string body starting at q (just past the opening quote) into UTF-8. */
static int json_decode(const char *q, char *out, int cap) {
    int i = 0;
    while (*q && *q != '"' && i < cap - 1) {
        if (*q != '\\') { out[i++] = *q++; continue; }
        q++;
        switch (*q) {
        case 'n': out[i++] = ' '; q++; break;
        case 't': out[i++] = ' '; q++; break;
        case 'r': case 'b': case 'f': q++; break;
        case 'u': {
            unsigned cp = hex4(q + 1); q += 5;
            if (cp >= 0xD800 && cp < 0xDC00 && q[0] == '\\' && q[1] == 'u') {   /* surrogate pair */
                unsigned lo = hex4(q + 2);
                if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); q += 6; }
            }
            i = put_utf8(out, i, cap, cp);
            break; }
        case 0: break;
        default: out[i++] = *q++; break;       /* \" \\ \/ */
        }
    }
    out[i] = 0;
    return i;
}

/* Copy the JSON string value of "key" found after p. */
static int json_str(const char *p, const char *key, char *out, int cap) {
    char pat[40]; const char *q;
    _snprintf(pat, sizeof pat, "\"%s\":\"", key);
    q = strstr(p, pat);
    if (!q) { out[0] = 0; return 0; }
    return json_decode(q + strlen(pat), out, cap);
}

/* "RunTimeTicks":<n> -> milliseconds, integer-only (ticks are 100 ns: drop 4 digits). */
static int json_ticks_ms(const char *p) {
    const char *q = strstr(p, "\"RunTimeTicks\":");
    char d[24]; int i = 0;
    if (!q) return 0;
    q += 15;
    while (*q >= '0' && *q <= '9' && i < 23) d[i++] = *q++;
    if (i <= 4) return 0;
    d[i - 4] = 0;
    return atoi(d);
}

/* Build \Flash2\Content\<id>>16>\<(id>>8)&0xff>\art\<id&0xff>_art.jpg (path the
   Now Playing art loader computes from the song id; see Lyra youtube_gem.cpp). */
static void art_path(DWORD id, wchar_t *dir, wchar_t *file) {
    swprintf(dir, L"\\Flash2\\Content\\%04x", (unsigned)(id >> 16));
    CreateDirectoryW(dir, NULL);
    swprintf(dir, L"\\Flash2\\Content\\%04x\\%02x", (unsigned)(id >> 16), (unsigned)((id >> 8) & 0xff));
    CreateDirectoryW(dir, NULL);
    swprintf(dir, L"\\Flash2\\Content\\%04x\\%02x\\art", (unsigned)(id >> 16), (unsigned)((id >> 8) & 0xff));
    CreateDirectoryW(dir, NULL);
    swprintf(file, L"%s\\%02x_art.jpg", dir, (unsigned)(id & 0xff));
}

static void set_meta(HMODULE zdk, const wchar_t *art_src, int dur_ms, const char *artist, const char *album) {
    QGetActiveIdxFn gidx = (QGetActiveIdxFn)GetProcAddress(zdk, L"ZDKMedia_Queue_GetActiveSongIndex");
    QGetSongAtFn gsong = (QGetSongAtFn)GetProcAddress(zdk, L"ZDKMedia_Queue_GetSongAtIndex");
    static wchar_t wa[160], wb[160]; wchar_t dir[80], file[100];
    int idx = -1; void *song = NULL;
    if (!gidx || !gsong) { flog("meta: queue functions missing"); return; }
    if (gidx(&idx) < 0 || idx < 0 || gsong(idx, &song) < 0 || !song) { flog("meta: no active song"); return; }
    flog("meta: active idx %d song 0x%08lx (kind 0x%02lx)", idx, (unsigned long)song, ((unsigned long)song) >> 24);
    if (artist[0]) { MultiByteToWideChar(CP_UTF8, 0, artist, -1, wa, 160); ZDK_SETPROP(song, 0x20002, (DWORD)wa); }
    if (album[0])  { MultiByteToWideChar(CP_UTF8, 0, album,  -1, wb, 160); ZDK_SETPROP(song, 0x20003, (DWORD)wb); }
    if (dur_ms > 0) ZDK_SETINT(song, 0x10003, (DWORD)dur_ms);
    flog("meta: artist/album/duration set");
    if (art_src) {
        art_path((DWORD)song, dir, file);
        if (CopyFileW(art_src, file, FALSE)) { ZDK_SETINT(song, 0x10008, 1); flog("art: attached"); say("album art attached\r\n"); }
        else flog("art: copy failed err %lu", GetLastError());
    }
}

/* Delete old cached songs; the one the player has open fails to delete, which is fine. */
static void clean_cache(void) {
    WIN32_FIND_DATAW fd; HANDLE h; wchar_t p[MAX_PATH];
    h = FindFirstFileW(L"\\flash2\\zunefin\\*.mp3", &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do { swprintf(p, L"\\flash2\\zunefin\\%s", fd.cFileName); DeleteFileW(p); } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static int run(const void *arg, int arg_len, void *out, int out_max, int *out_used) {
    char a[512], host[64] = "", key[80] = "", mode[16] = "fileurl", item[64] = "";
    char name[160] = "Jellyfin", artist[160] = "", album[160] = "", album_id[64] = "", path[600];
    wchar_t wurl[600], wtitle[200];
    int port = 8096, status, len, dur_ms = 0, have_art = 0;
    char *body;
    HMODULE zdk; PlaySongFromUrlFn play; int hr;

    o_buf = (char *)out; o_max = out_max; o_used = 0; o_buf[0] = 0;
    len = arg_len < (int)sizeof a - 1 ? arg_len : (int)sizeof a - 1;
    if (arg && len > 0) memcpy(a, arg, len);
    a[len > 0 ? len : 0] = 0;
    flog("=== jfplay start ===");
    sscanf(a, "%63s %d %79s %15s %63s", host, &port, key, mode, item);
    flog("args: host=%s port=%d mode=%s item=%s", host, port, mode, item);
    if (!host[0] || !key[0]) { say("usage: --arg \"<ip> <port> <api_key> [file|mp3|static|aac|url] [item_id]\"\r\n"); goto done; }

    /* 1. pick a song */
    if (item[0]) {
        _snprintf(path, sizeof path, "/Items?Ids=%s&Fields=Artists&api_key=%s", item, key);
    } else {
        _snprintf(path, sizeof path,
                  "/Items?IncludeItemTypes=Audio&Recursive=true&SortBy=Random&Limit=1&api_key=%s", key);
    }
    flog("lookup: sending");
    body = http_get(host, port, path, &status);
    flog("lookup: status %d, body %s", status, body ? "yes" : "no");
    if (!body) { say("item lookup failed\r\n"); goto done; }
    if (status != 200) {
        say("item lookup: HTTP %d (401 = bad API key)\r\n%.300s\r\n", status, body);
        free(body); goto done;
    }
    {
        const char *items = strstr(body, "\"Items\":[");
        if (!items || !json_str(items, "Id", item, sizeof item)) {
            say("no songs found in the reply:\r\n%.300s\r\n", body); free(body); goto done;
        }
        json_str(items, "Name", name, sizeof name);
        dur_ms = json_ticks_ms(items);
        json_str(items, "Album", album, sizeof album);
        json_str(items, "AlbumId", album_id, sizeof album_id);
        {                                      /* song artist: first of "Artists":["..."] */
            const char *ar = strstr(items, "\"Artists\":[\"");
            if (ar) json_decode(ar + 12, artist, sizeof artist);
        }
        if (!artist[0]) json_str(items, "AlbumArtist", artist, sizeof artist);
    }
    free(body);
    flog("parsed: id=%s dur_ms=%d", item, dur_ms);
    say("song: %s - %s [%s] (%d s)\r\nid:   %s\r\n", artist[0] ? artist : "?", name, album, dur_ms / 1000, item);

    /* file mode: download the MP3 to flash, then PlaySongFromFile */
    if (!strcmp(mode, "file") || !strcmp(mode, "fileurl")) {
        PlaySongFromFileFn pf; PlaySongFromUrlFn play; DWORD t0; long got; wchar_t cache[MAX_PATH];
        _snprintf(path, sizeof path,
                  "/Audio/%s/stream.mp3?audioCodec=mp3&audioBitRate=192000"
                  "&audioChannels=2&audioSampleRate=44100&api_key=%s", item, key);
        clean_cache();
        { wchar_t wid[64]; MultiByteToWideChar(CP_UTF8, 0, item, -1, wid, 64);
          swprintf(cache, L"\\flash2\\zunefin\\%s.mp3", wid); }
        flog("dl: start");
        t0 = GetTickCount();
        got = http_download(host, port, path, cache, &status);
        if (got <= 0 || status != 200) { say("download failed: HTTP %d, %ld bytes\r\n", status, got); goto done; }
        t0 = GetTickCount() - t0;
        flog("dl: done %ld bytes status %d", got, status);
        say("downloaded %ld KB in %lu ms (%lu KB/s)\r\n", got / 1024, t0, t0 ? (unsigned long)(got / t0) : 0);
        {   /* cover art first, so it's in place when Now Playing first draws.
               Jellyfin keeps covers on the album; songs rarely have their own. */
            const char *tries[2]; char ap[300]; int ast = 0, t; long ag = 0;
            tries[0] = album_id[0] ? album_id : item; tries[1] = item;
            for (t = 0; t < 2 && !have_art; t++) {
                if (t == 1 && tries[1] == tries[0]) break;
                _snprintf(ap, sizeof ap, "/Items/%s/Images/Primary?maxWidth=300&maxHeight=300&format=Jpg&quality=90&api_key=%s", tries[t], key);
                ag = http_download(host, port, ap, L"\\flash2\\zunefin\\art.jpg", &ast);
                flog("art: %s image: HTTP %d, %ld bytes", t == 0 && album_id[0] ? "album" : "song", ast, ag);
                have_art = (ag > 0 && ast == 200);
            }
            if (!have_art) { DeleteFileW(L"\\flash2\\zunefin\\art.jpg"); say("no cover art on the server for this song\r\n"); }
        }
        MultiByteToWideChar(CP_UTF8, 0, name, -1, wtitle, 200);
        zdk = LoadLibraryW(L"zdksystem.dll");
        play = zdk ? (PlaySongFromUrlFn)GetProcAddress(zdk, L"ZDKMedia_Queue_PlaySongFromURL") : NULL;
        pf = zdk ? (PlaySongFromFileFn)GetProcAddress(zdk, L"ZDKMedia_Queue_PlaySongFromFile") : NULL;
        hr = -1;
        if (play && strcmp(mode, "file") != 0) {          /* default "fileurl": streamed-song kind */
            flog("play: PlaySongFromURL(local path)");
            hr = play(wtitle, cache);
            flog("play: returned 0x%08x", hr);
            say("PlaySongFromURL(local file) returned 0x%08x\r\n", hr);
            if (hr >= 0) { Sleep(300); set_meta(zdk, have_art ? L"\\flash2\\zunefin\\art.jpg" : NULL, dur_ms, artist, album); }
        }
        if (hr < 0 && pf) {                                 /* fallback: library-kind song, no metadata */
            flog("play: calling PlaySongFromFile dur=%d", dur_ms);
            hr = pf(wtitle, cache, dur_ms);
            flog("play: returned 0x%08x", hr);
            say("PlaySongFromFile returned 0x%08x\r\n", hr);
        }
        say("now look at the Zune: is it playing?\r\n");
        goto done;
    }

    /* 2. build the stream URL */
    if (!strcmp(mode, "static"))
        _snprintf(path, sizeof path, "http://%s:%d/Audio/%s/stream?static=true&api_key=%s",
                  host, port, item, key);
    else if (!strcmp(mode, "aac"))
        _snprintf(path, sizeof path,
                  "http://%s:%d/Audio/%s/stream.aac?audioCodec=aac&audioBitRate=192000"
                  "&audioChannels=2&audioSampleRate=44100&api_key=%s", host, port, item, key);
    else
        _snprintf(path, sizeof path,
                  "http://%s:%d/Audio/%s/stream.mp3?audioCodec=mp3&audioBitRate=192000"
                  "&audioChannels=2&audioSampleRate=44100&api_key=%s", host, port, item, key);
    say("url:  %.120s...\r\n", path);
    if (!strcmp(mode, "url")) { say("(mode url: not playing)\r\n"); goto done; }

    /* 3. hand it to the Zune's player */
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wurl, 600);
    MultiByteToWideChar(CP_UTF8, 0, name, -1, wtitle, 200);
    zdk = LoadLibraryW(L"zdksystem.dll");
    play = zdk ? (PlaySongFromUrlFn)GetProcAddress(zdk, L"ZDKMedia_Queue_PlaySongFromURL") : NULL;
    if (!play) { say("could not find ZDKMedia_Queue_PlaySongFromURL (err %lu)\r\n", GetLastError()); goto done; }
    hr = play(wtitle, wurl);
    say("PlaySongFromURL(mode %s) returned 0x%08x (0 = accepted)\r\n", mode, hr);
    say("now look at the Zune: is it playing?\r\n");

done:
    flog("=== jfplay end ===");
    if (out_used) *out_used = o_used;
    return 0;
}

#ifdef JF_EXE
/* Standalone program: same run(), output goes to \flash2\zunefin-out.txt. */
int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPWSTR cmdline, int show) {
    static char outbuf[8192]; char args[512]; int used = 0; WSADATA wsa;
    HANDLE h; DWORD w;
    (void)hi; (void)hp; (void)show;
    DeleteFileW(L"\\flash2\\zunefin.log");
    flog("exe started");
    WideCharToMultiByte(CP_UTF8, 0, cmdline, -1, args, sizeof args, NULL, NULL);
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) { flog("WSAStartup failed"); return 1; }
    run(args, (int)strlen(args), outbuf, sizeof outbuf, &used);
    WSACleanup();
    h = CreateFileW(L"\\flash2\\zunefin-out.txt", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) { WriteFile(h, outbuf, used, &w, NULL); CloseHandle(h); }
    flog("exe finished");
    return 0;
}
#else
EXPORT int Run(const void *arg, int arg_len, void *out, int out_max, int *out_used)
{ return run(arg, arg_len, out, out_max, out_used); }

BOOL WINAPI DllMain(HANDLE h, DWORD reason, LPVOID r) { return TRUE; }
#endif
