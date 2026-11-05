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
#include <tlhelp32.h>

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
static HANDLE g_stop;       /* signalled when a newer jfplay wants us gone */
static HANDLE g_play_stop;  /* signalled to cancel the current album playback (serve mode) */
static int stopping(void) {
    return (g_stop && WaitForSingleObject(g_stop, 0) == WAIT_OBJECT_0) ||
           (g_play_stop && WaitForSingleObject(g_play_stop, 0) == WAIT_OBJECT_0);
}
static char g_status[200] = "idle";   /* shown by the web pages */

/* Write what we have so far to \flash2\zunefin-out.txt (album mode runs for minutes). */
static void flush_out(void) {
    HANDLE h = CreateFileW(L"\\flash2\\zunefin-out.txt", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD w;
    if (h == INVALID_HANDLE_VALUE) return;
    WriteFile(h, o_buf, o_used, &w, NULL);
    CloseHandle(h);
}
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
    SOCKET s; struct sockaddr_in a; char req[700]; int n, cap = 524288, len = 0;
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
    while (hdr && !stopping() && (n = recv(s, buf, 8192, 0)) > 0) {
        if (!WriteFile(f, buf, n, &w, NULL) || (int)w != n) { say("write failed (disk full?)\r\n"); total = -1; break; }
        total += n;
        if ((total & 0xFFFFF) < n) flog("dl: %ld KB", total / 1024);
    }
    CloseHandle(f); closesocket(s); free(buf);
    if (stopping()) { DeleteFileW(dest); return -1; }
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


/* ===================== album mode: native queue ===================== */

typedef int (*CreateSongFn)(DWORD kind, DWORD *out_id);
typedef int (*MoveToFn)(int pos);
#define CREATE_SONG ((CreateSongFn)0x4198484c)   /* create(kind, &id): kind 0xfd runtime song */

/* servicesd's runtime (URL-song) now-playing queue, firmware 4.5 (from Lyra yt_queue.c):
   id storage std::vector<u32> at +0x60/64/68, play-order at +0x6c/70/74, cursor +0xa4. */
#define QOBJ          0x41a525d4u
#define OFF_ID_BEGIN  0x60
#define OFF_ID_END    0x64
#define OFF_ID_CAP    0x68
#define OFF_ORD_BEGIN 0x6c
#define OFF_ORD_END   0x70
#define OFF_ORD_CAP   0x74
#define OFF_CUR       0xa4

static DWORD find_servicesd(void) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 pe; DWORD pid = 0;
    if (snap == INVALID_HANDLE_VALUE) return 0;
    pe.dwSize = sizeof pe;
    if (Process32First(snap, &pe)) {
        do { if (wcsstr(pe.szExeFile, L"servicesd")) { pid = pe.th32ProcessID; break; } }
        while (Process32Next(snap, &pe));
    }
    CloseToolhelp32Snapshot(snap);
    return pid;
}

static HANDLE q_open(void) {
    DWORD pid = find_servicesd(); HANDLE h;
    if (!pid) { flog("queue: servicesd not found"); return NULL; }
    h = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!h) flog("queue: OpenProcess(servicesd) failed err %lu", GetLastError());
    return h;
}
static int rmem(HANDLE h, DWORD va, void *b, DWORD n) { DWORD g = 0; return (ReadProcessMemory(h, (LPCVOID)va, b, n, &g) && g == n) ? 0 : -1; }
static int wmem(HANDLE h, DWORD va, const void *b, DWORD n) { DWORD g = 0; return (WriteProcessMemory(h, (LPVOID)va, (LPCVOID)b, n, &g) && g == n) ? 0 : -1; }

static int q_count(HANDLE h) {
    DWORD b, e;
    if (rmem(h, QOBJ + OFF_ORD_BEGIN, &b, 4) || rmem(h, QOBJ + OFF_ORD_END, &e, 4)) { flog("queue: read failed err %lu", GetLastError()); return -1; }
    return e >= b ? (int)((e - b) / 4) : -1;
}
static int q_push_id(HANDLE h, DWORD id) {
    DWORD b, e, c, v;
    if (rmem(h, QOBJ + OFF_ID_BEGIN, &b, 4) || rmem(h, QOBJ + OFF_ID_END, &e, 4) || rmem(h, QOBJ + OFF_ID_CAP, &c, 4)) return -1;
    if (!b || e < b || e + 4 > c) { flog("queue: id storage full (cap %lu)", (unsigned long)((c - b) / 4)); return -1; }
    if (wmem(h, e, &id, 4)) return -1;
    v = e + 4;
    if (wmem(h, QOBJ + OFF_ID_END, &v, 4)) return -1;
    return (int)((e - b) / 4);
}
static int q_set_order(HANDLE h, const DWORD *order, int n, int cur) {
    DWORD b, c, v; int i;
    if (rmem(h, QOBJ + OFF_ORD_BEGIN, &b, 4) || rmem(h, QOBJ + OFF_ORD_CAP, &c, 4)) return -1;
    if (!b || b + (DWORD)n * 4 > c) { flog("queue: order capacity too small for %d", n); return -1; }
    for (i = 0; i < n; i++) if (wmem(h, b + (DWORD)i * 4, &order[i], 4)) return -1;
    v = b + (DWORD)n * 4;  if (wmem(h, QOBJ + OFF_ORD_END, &v, 4)) return -1;
    v = (DWORD)cur;        if (wmem(h, QOBJ + OFF_CUR, &v, 4)) return -1;
    return 0;
}

typedef struct { char id[40]; char name[160]; char artist[160]; int dur_ms; DWORD song; } Track;
#define MAX_TRACKS 60
static Track g_tr[MAX_TRACKS];   /* the album being played */

/* Next top-level {...} object in an array at *pp; returns a malloc'd copy or NULL. */
static char *json_next_obj(const char **pp) {
    const char *p = *pp, *st; int depth = 0, instr = 0, len; char *obj;
    while (*p && *p != '{' && *p != ']') p++;
    if (*p != '{') { *pp = p; return NULL; }
    st = p;
    for (; *p; p++) {
        if (instr) { if (*p == '\\' && p[1]) p++; else if (*p == '"') instr = 0; continue; }
        if (*p == '"') instr = 1;
        else if (*p == '{') depth++;
        else if (*p == '}' && --depth == 0) { p++; break; }
    }
    *pp = p;
    len = (int)(p - st);
    obj = (char *)malloc(len + 1);
    if (obj) { memcpy(obj, st, len); obj[len] = 0; }
    return obj;
}

/* Pull each track's fields out of an Items array. */
static int parse_tracks_into(const char *body, Track *tr, int max) {
    const char *p = strstr(body, "\"Items\":["); int n = 0; char *obj;
    if (!p) return 0;
    p += 9;
    while (n < max && (obj = json_next_obj(&p)) != NULL) {
        if (json_str(obj, "Id", tr[n].id, sizeof tr[n].id)) {
            const char *ar = strstr(obj, "\"Artists\":[\"");
            json_str(obj, "Name", tr[n].name, sizeof tr[n].name);
            tr[n].dur_ms = json_ticks_ms(obj);
            tr[n].artist[0] = 0;
            if (ar) json_decode(ar + 12, tr[n].artist, sizeof tr[n].artist);
            if (!tr[n].artist[0]) json_str(obj, "AlbumArtist", tr[n].artist, sizeof tr[n].artist);
            tr[n].song = 0;
            n++;
        }
        free(obj);
    }
    return n;
}

static int parse_tracks(const char *body) { return parse_tracks_into(body, g_tr, MAX_TRACKS); }

static void track_path(const Track *t, wchar_t *out) {
    wchar_t wid[48];
    MultiByteToWideChar(CP_UTF8, 0, t->id, -1, wid, 48);
    swprintf(out, L"\\flash2\\zunefin\\%s.mp3", wid);
}

static long download_track(const char *host, int port, const char *key, const Track *t) {
    char p[400]; wchar_t dest[MAX_PATH]; int st; long got;
    _snprintf(p, sizeof p, "/Audio/%s/stream.mp3?audioCodec=mp3&audioBitRate=192000"
              "&audioChannels=2&audioSampleRate=44100&api_key=%s", t->id, key);
    track_path(t, dest);
    got = http_download(host, port, p, dest, &st);
    if (st != 200) got = -1;
    return got;
}

static void attach_art(DWORD song, const wchar_t *art) {
    wchar_t dir[80], file[100];
    art_path(song, dir, file);
    if (CopyFileW(art, file, FALSE)) ZDK_SETINT((void *)song, 0x10008, 1);
}

static void album_play(const char *host, int port, const char *key, const char *album_in, const char *start_id) {
    char path[600], album_id[64] = "", album[160] = "", album_artist[160] = "";
    char *body; int status, n, k, j, have_art = 0, queued = 0;
    HMODULE zdk; PlaySongFromUrlFn play; HANDLE q = NULL;
    static wchar_t wt[200], wp[MAX_PATH], wa[160], wb[160];
    DWORD order[MAX_TRACKS]; int m = 0, st = 0, cur = 0;
    const wchar_t *ART = L"\\flash2\\zunefin\\art.jpg";

    /* 1. which album */
    if (album_in && album_in[0] && strcmp(album_in, "random") != 0)
        _snprintf(path, sizeof path, "/Items?Ids=%s&api_key=%s", album_in, key);
    else
        _snprintf(path, sizeof path, "/Items?IncludeItemTypes=MusicAlbum&Recursive=true&SortBy=Random&Limit=10&Fields=ChildCount&EnableImages=false&EnableUserData=false&api_key=%s", key);
    body = http_get(host, port, path, &status);
    if (!body || status != 200) { say("album lookup failed (HTTP %d)\r\n", status); free(body); return; }
    {
        /* walk the candidates; take the first with 3+ songs, else the first one */
        const char *p = strstr(body, "\"Items\":["); char *obj; int first = 1;
        if (p) p += 9;
        while (p && (obj = json_next_obj(&p)) != NULL) {
            const char *cc = strstr(obj, "\"ChildCount\":");
            int good = (album_in && album_in[0] && strcmp(album_in, "random") != 0) || (cc && atoi(cc + 13) >= 3);
            if (first || good) {
                json_str(obj, "Id", album_id, sizeof album_id); json_str(obj, "Name", album, sizeof album);
                json_str(obj, "AlbumArtist", album_artist, sizeof album_artist);
            }
            first = 0;
            free(obj);
            if (good) break;
        }
    }
    free(body);
    if (!album_id[0]) { say("no album found\r\n"); return; }
    flog("album: %s [%s] by %s", album, album_id, album_artist);

    /* 2. its tracks, in disc/track order */
    _snprintf(path, sizeof path, "/Items?ParentId=%s&IncludeItemTypes=Audio&Recursive=true"
              "&SortBy=ParentIndexNumber,IndexNumber,SortName&EnableImages=false&EnableUserData=false"
              "&Limit=%d&api_key=%s", album_id, MAX_TRACKS, key);
    body = http_get(host, port, path, &status);
    if (!body || status != 200) { say("track list failed (HTTP %d)\r\n", status); free(body); return; }
    n = parse_tracks(body);
    free(body);
    flog("album: %d tracks", n);
    say("album: %s - %s (%d tracks)\r\n", album_artist, album, n);
    if (n == 0) return;
    for (k = 0; k < n; k++) say("  %2d. %s (%d:%02d)\r\n", k + 1, g_tr[k].name, g_tr[k].dur_ms / 60000, (g_tr[k].dur_ms / 1000) % 60);
    if (start_id && start_id[0]) for (k = 0; k < n; k++) if (!strcmp(g_tr[k].id, start_id)) { st = k; break; }
    _snprintf(g_status, sizeof g_status, "Loading %s - %s", album_artist, g_tr[st].name);

    clean_cache();

    /* 3. cover (album-level), then the first track */
    {
        char ap[300]; int ast; long ag;
        const char *src_id[2]; int t;
        src_id[0] = album_id; src_id[1] = g_tr[st].id;     /* album cover, else the track's embedded art */
        for (t = 0; t < 2 && !have_art; t++) {
            _snprintf(ap, sizeof ap, "/Items/%s/Images/Primary?maxWidth=300&maxHeight=300&format=Jpg&quality=90&api_key=%s", src_id[t], key);
            ag = http_download(host, port, ap, ART, &ast);
            have_art = (ag > 0 && ast == 200);
            flog("art: %s: HTTP %d, %ld bytes", t ? "track 1" : "album", ast, ag);
        }
        if (!have_art) DeleteFileW(ART);
    }
    {
        DWORD t0 = GetTickCount(); long got = download_track(host, port, key, &g_tr[st]);
        flog("dl %d/%d: %ld bytes in %lu ms", st + 1, n, got, GetTickCount() - t0);
        if (got <= 0) { say("download of track %d failed\r\n", st + 1); _snprintf(g_status, sizeof g_status, "Download failed"); return; }
    }

    /* 4. play track 1 */
    zdk = LoadLibraryW(L"zdksystem.dll");
    play = zdk ? (PlaySongFromUrlFn)GetProcAddress(zdk, L"ZDKMedia_Queue_PlaySongFromURL") : NULL;
    if (!play) { say("PlaySongFromURL missing\r\n"); return; }
    if (stopping()) return;
    MultiByteToWideChar(CP_UTF8, 0, g_tr[st].name, -1, wt, 200);
    track_path(&g_tr[st], wp);
    status = play(wt, wp);
    flog("play: track %d returned 0x%08x", st + 1, status);
    if (status < 0) { say("player refused track %d (0x%08x)\r\n", st + 1, status); return; }
    Sleep(300);
    set_meta(zdk, have_art ? ART : NULL, g_tr[st].dur_ms, g_tr[st].artist, album);
    say("playing track %d\r\n", st + 1);
    _snprintf(g_status, sizeof g_status, "Playing %s - %s", album_artist, album);

    /* 5. create + queue tracks 2..n now; their files download below */
    MultiByteToWideChar(CP_UTF8, 0, album, -1, wb, 160);
    if (n > 1 && (q = q_open()) != NULL) {
        int tries, c = -1;
        for (tries = 0; tries < 40; tries++) { c = q_count(q); if (c >= 1) break; Sleep(100); }
        flog("queue: count %d after %d tries", c, tries);
        if (c >= 1) {
            for (k = 0; k < n; k++) {
                DWORD id = 0; int si;
                if (k == st) { cur = m; order[m++] = 0; continue; }   /* playing song sits at storage 0 */
                if (CREATE_SONG(0xfd, &id) < 0 || !id) { flog("queue: create failed at %d", k + 1); break; }
                MultiByteToWideChar(CP_UTF8, 0, g_tr[k].name, -1, wt, 200);
                MultiByteToWideChar(CP_UTF8, 0, g_tr[k].artist, -1, wa, 160);
                track_path(&g_tr[k], wp);
                ZDK_SETPROP((void *)id, 0x20001, (DWORD)wt);
                ZDK_SETPROP((void *)id, 0x2000a, (DWORD)wp);
                if (wa[0]) ZDK_SETPROP((void *)id, 0x20002, (DWORD)wa);
                if (wb[0]) ZDK_SETPROP((void *)id, 0x20003, (DWORD)wb);
                if (g_tr[k].dur_ms > 0) ZDK_SETINT((void *)id, 0x10003, (DWORD)g_tr[k].dur_ms);
                si = q_push_id(q, id);
                if (si < 0) { flog("queue: push failed at %d", k + 1); break; }
                order[m++] = (DWORD)si;
                g_tr[k].song = id;
            }
            if (m > 1 && q_set_order(q, order, m, cur) == 0) {
                MoveToFn mv = (MoveToFn)GetProcAddress(zdk, L"ZDKMedia_Queue_MoveTo");
                queued = m;
                flog("queue: %d songs queued, cursor %d", m, cur);
                if (mv) mv(cur);   /* sync the live cursor (proven YouTube-mod sequence) */
            }
            if (have_art) for (k = 0; k < n; k++) if (g_tr[k].song) attach_art(g_tr[k].song, ART);
        }
        CloseHandle(q);
    }
    if (queued) say("queued %d songs: use next/previous on the Zune\r\n", queued);
    else        say("couldn't add to the Zune's queue (see log); only track 1 will play\r\n");
    flush_out();

    /* 6. download the rest in order, well ahead of playback (~5x real time) */
    for (j = 1; j < n && !stopping(); j++) {
        DWORD t0; long got;
        k = (st + j) % n;
        _snprintf(g_status, sizeof g_status, "Playing %s - %s (downloading %d of %d)", album_artist, album, j + 1, n);
        t0 = GetTickCount(); got = download_track(host, port, key, &g_tr[k]);
        flog("dl %d/%d: %ld bytes in %lu ms", k + 1, n, got, GetTickCount() - t0);
        if (got <= 0) say("download of track %d failed\r\n", k + 1);
    }
    if (stopping()) { flog("stopped by a newer jfplay"); say("stopped: a newer jfplay took over\r\n"); return; }
    say("all %d tracks downloaded\r\n", n);
    _snprintf(g_status, sizeof g_status, "Playing %s - %s (all downloaded)", album_artist, album);
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
    if (!host[0] || !key[0]) { say("usage: --arg \"<ip> <port> <api_key> [fileurl|album|file|mp3|static|aac|url] [item_id]\"\r\n"); goto done; }
    if (!strcmp(mode, "album")) { album_play(host, port, key, item, NULL); goto done; }

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
#include "serve.inc.c"

/* Standalone program.
 *   jfplay.exe <ip> <port> <key> serve   save settings, then run the browse server
 *   jfplay.exe                           (Lyra boot daemon) run the server from saved settings
 *   jfplay.exe <ip> <port> <key> <mode> [id]   one-shot test modes, output to zunefin-out.txt */
int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPWSTR cmdline, int show) {
    static char outbuf[8192]; char args[512], mode[16] = ""; int used = 0; WSADATA wsa;
    HANDLE h; DWORD w;
    HANDLE mtx;
    (void)hi; (void)hp; (void)show;
    /* one jfplay at a time: ask a running one to stop, then wait for it */
    g_stop = CreateEventW(NULL, TRUE, FALSE, L"ZuneFinStop");
    mtx = CreateMutexW(NULL, FALSE, L"ZuneFinPlayer");
    if (GetLastError() == ERROR_ALREADY_EXISTS) SetEvent(g_stop);
    if (mtx && WaitForSingleObject(mtx, 30000) == WAIT_TIMEOUT) return 2;
    ResetEvent(g_stop);
    DeleteFileW(L"\\flash2\\zunefin.log");
    flog("exe started");
    WideCharToMultiByte(CP_UTF8, 0, cmdline ? cmdline : L"", -1, args, sizeof args, NULL, NULL);
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) { flog("WSAStartup failed"); return 1; }

    sscanf(args, "%*s %*d %*s %15s", mode);
    if (!args[0] || !strcmp(mode, "serve")) {
        if (args[0] && sscanf(args, "%63s %d %79s", g_host, &g_port, g_key) == 3) save_config();
        else if (!load_config()) { flog("serve: no settings (run: zf serve once)"); goto out; }
        serve_main();
        goto out;
    }
    run(args, (int)strlen(args), outbuf, sizeof outbuf, &used);
    h = CreateFileW(L"\\flash2\\zunefin-out.txt", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) { WriteFile(h, outbuf, used, &w, NULL); CloseHandle(h); }
out:
    WSACleanup();
    flog("exe finished");
    if (mtx) { ReleaseMutex(mtx); CloseHandle(mtx); }
    return 0;
}
#else
EXPORT int Run(const void *arg, int arg_len, void *out, int out_max, int *out_used)
{ return run(arg, arg_len, out, out_max, out_used); }

BOOL WINAPI DllMain(HANDLE h, DWORD reason, LPVOID r) { return TRUE; }
#endif
