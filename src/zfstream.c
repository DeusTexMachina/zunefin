/* zfstream.c: DirectShow source filter for jf://<itemId> on the Zune HD.
 *
 * Registered as the "Source Filter" for the jf: URL scheme (see register_stream()
 * in jfplay.c). zmedia CoCreates it inside the player service when the queue plays
 * a jf:// song. Load() opens Jellyfin's AAC (ADTS) stream for the item; a fetch
 * thread fills a ring buffer; a push thread strips the ADTS headers and delivers
 * raw AAC frames to the Zune's NVIDIA AAC decoder with the media type Lyra's
 * YouTube mod proved works (MEDIATYPE_Audio / {0000160A-...} / WAVEFORMATEX 2ch 44.1k).
 *
 * Server settings come from \flash2\zunefin\config.txt ("host port key"), written
 * by the browse server, so the API key never appears in a URL.
 *
 * Runs inside a system process: every fault reboots the Zune. Keep it simple,
 * check everything, and log each step to \flash2\zunefin-stream.log.
 */
#include <windows.h>
#include <winsock2.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stddef.h>

/* ---------- logging ---------- */

static void slog(const char *fmt, ...) {
    char line[300]; va_list ap; int n; HANDLE h; DWORD w;
    va_start(ap, fmt); n = _vsnprintf(line, sizeof line - 3, fmt, ap); va_end(ap);
    if (n < 0) n = sizeof line - 3;
    line[n++] = '\r'; line[n++] = '\n';
    h = CreateFileW(L"\\flash2\\zunefin-stream.log", GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    if (GetFileSize(h, NULL) > 256 * 1024) { SetFilePointer(h, 0, NULL, FILE_BEGIN); SetEndOfFile(h); }
    SetFilePointer(h, 0, NULL, FILE_END);
    WriteFile(h, line, n, &w, NULL);
    CloseHandle(h);
}

/* ---------- COM / DirectShow definitions (just what we use) ---------- */

typedef struct { GUID majortype, subtype; int bFixed, bTemporal; unsigned long lSampleSize; GUID formattype; void *pUnk; unsigned long cbFormat; unsigned char *pbFormat; } AMMT;
typedef struct { void *pFilter; int dir; wchar_t achName[128]; } PININFO;
typedef struct { wchar_t achName[128]; void *pGraph; } FILTERINFO;
typedef struct { long cBuffers, cbBuffer, cbAlign, cbPrefix; } ALLOCPROPS;

static const GUID IID_IUnknown_         = {0x00000000,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
static const GUID IID_IClassFactory_    = {0x00000001,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
static const GUID IID_IPersist_         = {0x0000010c,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
static const GUID IID_IMediaFilter_     = {0x56a86899,0x0ad4,0x11ce,{0xb0,0x3a,0x00,0x20,0xaf,0x0b,0xa7,0x70}};
static const GUID IID_IBaseFilter_      = {0x56a86895,0x0ad4,0x11ce,{0xb0,0x3a,0x00,0x20,0xaf,0x0b,0xa7,0x70}};
static const GUID IID_IFileSourceFilter_= {0x56a868a6,0x0ad4,0x11ce,{0xb0,0x3a,0x00,0x20,0xaf,0x0b,0xa7,0x70}};
static const GUID IID_IPin_             = {0x56a86891,0x0ad4,0x11ce,{0xb0,0x3a,0x00,0x20,0xaf,0x0b,0xa7,0x70}};
static const GUID IID_IEnumPins_        = {0x56a86892,0x0ad4,0x11ce,{0xb0,0x3a,0x00,0x20,0xaf,0x0b,0xa7,0x70}};
static const GUID IID_IEnumMediaTypes_  = {0x89c31040,0x846b,0x11ce,{0x97,0xd3,0x00,0xaa,0x00,0x55,0x59,0x5a}};
static const GUID IID_IMemInputPin_     = {0x56a8689d,0x0ad4,0x11ce,{0xb0,0x3a,0x00,0x20,0xaf,0x0b,0xa7,0x70}};
static const GUID IID_IMediaSeeking_   = {0x36b73880,0xc2c8,0x11cf,{0x8b,0x46,0x00,0x80,0x5f,0x6c,0xef,0x60}};
static const GUID TIME_FORMAT_MEDIA_TIME_ = {0x7b785574,0x8c82,0x11cf,{0xbc,0x0c,0x00,0xaa,0x00,0xac,0x74,0xf6}};
static const GUID MT_Audio  = {0x73647561,0x0000,0x0010,{0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}};
static const GUID MT_AAC    = {0x0000160a,0x0000,0x0010,{0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}};
static const GUID FMT_WFX   = {0x05589f81,0xc356,0x11ce,{0xbf,0x01,0x00,0xaa,0x00,0x55,0x59,0x5a}};
/* {C0FFEE10-2026-4A6E-B0DE-5A554E454649} "ZUNEFI" */
static const GUID CLSID_ZFStream = {0xC0FFEE10,0x2026,0x4A6E,{0xB0,0xDE,0x5A,0x55,0x4E,0x45,0x46,0x49}};

/* WAVEFORMATEX template: tag 0x160a, 2 ch, 44100 Hz, cbSize 0. Each filter patches its own copy
   from the first ADTS header. */
static const unsigned char g_wfx_default[18] = {0x0a,0x16, 2,0, 0x44,0xac,0,0, 0,0,0,0, 0,0, 0,0, 0,0};

#define VT(o) (*(void ***)(o))
static int geq(const GUID *a, const GUID *b) { return memcmp(a, b, 16) == 0; }
static LONG g_objs;

typedef void *(*CoTaskMemAllocFn)(unsigned long);
static CoTaskMemAllocFn g_ctma;

static void fill_mt(AMMT *m, unsigned char *wfx) {
    memset(m, 0, sizeof *m);
    m->majortype = MT_Audio; m->subtype = MT_AAC; m->lSampleSize = 1;
    m->formattype = FMT_WFX; m->cbFormat = 18; m->pbFormat = wfx;
}
static AMMT *mt_alloc(const unsigned char *wfx) {
    AMMT *m; unsigned char *pb;
    if (!g_ctma) { HMODULE ole = LoadLibraryW(L"ole32.dll"); if (ole) g_ctma = (CoTaskMemAllocFn)GetProcAddressW(ole, L"CoTaskMemAlloc"); }
    if (!g_ctma) return NULL;
    m = (AMMT *)g_ctma(sizeof(AMMT));
    if (!m) return NULL;
    fill_mt(m, NULL);
    pb = (unsigned char *)g_ctma(18);
    if (pb) { memcpy(pb, wfx, 18); m->pbFormat = pb; }
    return m;
}

/* ---------- objects ---------- */

#define RING (512 * 1024)        /* ~21 s of 192 kbps AAC */
#define PREBUF (24 * 1024)       /* ~1 s before the first frame goes out */

typedef struct Filter Filter;
typedef struct { void *v; LONG ref; Filter *f; void *peer, *mem, *alloc; AMMT mt; } Pin;
struct Filter {
    void *vF; void *vFSF;
    LONG ref; Pin *pin; void *graph; int state;
    SOCKET sock;
    unsigned char *ring; volatile LONG wr, rd; volatile LONG done, err;
    unsigned char wfx[18];
    void *vMS;                                   /* IMediaSeeking (shared by filter and pin) */
    char item[48];
    LONGLONG dur, base;                          /* 100 ns units, same as Jellyfin ticks */
    volatile LONG frames, discont;
    CRITICAL_SECTION seek_cs;
    HANDLE hStop, hFetch, hPush;
    wchar_t cur[260];
};
typedef struct { void *v; LONG ref; int idx; const unsigned char *wfx; } EnumMT;
typedef struct { void *v; LONG ref; Filter *f; int idx; } EnumPins;
typedef struct { void *v; LONG ref; } Factory;

static void *g_FV[15], *g_SV[5], *g_PV[18], *g_EPV[7], *g_EMV[7], *g_CFV[5], *g_MSV[20];
static Factory g_factory;
static Filter *from_fsf(void *o) { return (Filter *)((char *)o - sizeof(void *)); }

/* IEnumMediaTypes */
static HRESULT EM_QI(void *o, const GUID *i, void **out) { EnumMT *e = o; if (geq(i, &IID_IUnknown_) || geq(i, &IID_IEnumMediaTypes_)) { *out = o; InterlockedIncrement(&e->ref); return 0; } *out = 0; return 0x80004002; }
static ULONG EM_AR(void *o) { return InterlockedIncrement(&((EnumMT *)o)->ref); }
static ULONG EM_RL(void *o) { EnumMT *e = o; LONG r = InterlockedDecrement(&e->ref); if (!r) LocalFree(e); return r; }
static HRESULT EM_Next(void *o, ULONG c, AMMT **out, ULONG *got) { EnumMT *e = o; ULONG n = 0; while (n < c && e->idx < 1) { AMMT *m = mt_alloc(e->wfx); if (!m) break; out[n++] = m; e->idx++; } if (got) *got = n; return n == c ? 0 : 1; }
static HRESULT EM_Skip(void *o, ULONG c) { ((EnumMT *)o)->idx += c; return 0; }
static HRESULT EM_Reset(void *o) { ((EnumMT *)o)->idx = 0; return 0; }
static HRESULT EM_Clone(void *o, void **p) { (void)o; *p = 0; return 0x80004001; }

/* IEnumPins */
static HRESULT EP_QI(void *o, const GUID *i, void **out) { EnumPins *e = o; if (geq(i, &IID_IUnknown_) || geq(i, &IID_IEnumPins_)) { *out = o; InterlockedIncrement(&e->ref); return 0; } *out = 0; return 0x80004002; }
static ULONG EP_AR(void *o) { return InterlockedIncrement(&((EnumPins *)o)->ref); }
static ULONG EP_RL(void *o) { EnumPins *e = o; LONG r = InterlockedDecrement(&e->ref); if (!r) LocalFree(e); return r; }
static HRESULT EP_Next(void *o, ULONG c, void **out, ULONG *got) { EnumPins *e = o; ULONG n = 0; while (n < c && e->idx < 1) { Pin *p = e->f->pin; out[n++] = &p->v; InterlockedIncrement(&p->ref); e->idx++; } if (got) *got = n; return n == c ? 0 : 1; }
static HRESULT EP_Skip(void *o, ULONG c) { ((EnumPins *)o)->idx += c; return 0; }
static HRESULT EP_Reset(void *o) { ((EnumPins *)o)->idx = 0; return 0; }
static HRESULT EP_Clone(void *o, void **p) { (void)o; *p = 0; return 0x80004001; }

/* IPin (output, push model) */
static ULONG F_AR(void *o);
static HRESULT P_QI(void *o, const GUID *i, void **out) {
    Pin *p = o;
    if (geq(i, &IID_IUnknown_) || geq(i, &IID_IPin_)) { *out = &p->v; InterlockedIncrement(&p->ref); return 0; }
    if (geq(i, &IID_IMediaSeeking_)) { *out = &p->f->vMS; InterlockedIncrement(&p->f->ref); slog("seek: pin QI(IMediaSeeking)"); return 0; }
    *out = 0; return 0x80004002;
}
static ULONG P_AR(void *o) { return InterlockedIncrement(&((Pin *)o)->ref); }
static ULONG P_RL(void *o) { return InterlockedDecrement(&((Pin *)o)->ref); }
static HRESULT P_Connect(void *o, void *recv, const AMMT *pmt) {
    Pin *p = o; HRESULT hr; void *a = NULL; ALLOCPROPS want, got;
    (void)pmt;
    hr = ((HRESULT (*)(void *, void *, const AMMT *))VT(recv)[4])(recv, &p->v, &p->mt);   /* ReceiveConnection */
    slog("pin: ReceiveConnection 0x%08lx", (unsigned long)hr);
    if (hr) return hr;
    p->peer = recv;
    hr = ((HRESULT (*)(void *, const GUID *, void **))VT(recv)[0])(recv, &IID_IMemInputPin_, &p->mem);
    if (hr || !p->mem) { slog("pin: no IMemInputPin 0x%08lx", (unsigned long)hr); return hr ? hr : 0x80004002; }
    hr = ((HRESULT (*)(void *, void **))VT(p->mem)[3])(p->mem, &a);                       /* GetAllocator */
    if (hr || !a) { slog("pin: no allocator 0x%08lx", (unsigned long)hr); return hr ? hr : 0x80004005; }
    p->alloc = a;
    memset(&want, 0, sizeof want); want.cBuffers = 8; want.cbBuffer = 16384; want.cbAlign = 1;
    hr = ((HRESULT (*)(void *, ALLOCPROPS *, ALLOCPROPS *))VT(a)[3])(a, &want, &got);       /* SetProperties */
    hr = ((HRESULT (*)(void *, void *, int))VT(p->mem)[4])(p->mem, a, 0);                  /* NotifyAllocator */
    hr = ((HRESULT (*)(void *))VT(a)[5])(a);                                                /* Commit */
    slog("pin: connected, allocator committed 0x%08lx", (unsigned long)hr);
    return 0;
}
static HRESULT P_ReceiveConnection(void *o, void *c, const AMMT *m) { (void)o; (void)c; (void)m; return 0x80004001; }
static HRESULT P_Disconnect(void *o) { ((Pin *)o)->peer = 0; return 0; }
static HRESULT P_ConnectedTo(void *o, void **pp) { Pin *p = o; if (!p->peer) { *pp = 0; return 0x80040209; } *pp = p->peer; ((ULONG (*)(void *))VT(p->peer)[1])(p->peer); return 0; }
static HRESULT P_ConnectionMediaType(void *o, AMMT *m) { *m = ((Pin *)o)->mt; return 0; }
static HRESULT P_QueryPinInfo(void *o, PININFO *info) { Pin *p = o; info->pFilter = &p->f->vF; F_AR(&p->f->vF); info->dir = 1; wcscpy(info->achName, L"Out"); return 0; }
static HRESULT P_QueryDirection(void *o, int *d) { (void)o; *d = 1; return 0; }
static HRESULT P_QueryId(void *o, wchar_t **id) { (void)o; *id = 0; return 0x80004001; }
static HRESULT P_QueryAccept(void *o, const AMMT *m) { (void)o; return geq(&m->majortype, &MT_Audio) ? 0 : 1; }
static HRESULT P_EnumMediaTypes(void *o, void **e) { EnumMT *em = (EnumMT *)LocalAlloc(LPTR, sizeof(EnumMT)); if (!em) return 0x8007000e; em->v = g_EMV; em->ref = 1; em->wfx = ((Pin *)o)->f->wfx; *e = em; return 0; }
static HRESULT P_QueryInternalConnections(void *o, void **a, ULONG *n) { (void)o; (void)a; if (n) *n = 0; return 0; }
static HRESULT P_EndOfStream(void *o) { (void)o; return 0; }
static HRESULT P_BeginFlush(void *o) { (void)o; return 0; }
static HRESULT P_EndFlush(void *o) { (void)o; return 0; }
static HRESULT P_NewSegment(void *o, LONGLONG a, LONGLONG b, double r) { (void)o; (void)a; (void)b; (void)r; return 0; }

/* ---------- network: open Jellyfin's ADTS stream ---------- */

static int read_config(char *host, int *port, char *key) {
    HANDLE h = CreateFileW(L"\\flash2\\zunefin\\config.txt", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    char buf[300]; DWORD got = 0;
    if (h == INVALID_HANDLE_VALUE) return 0;
    ReadFile(h, buf, sizeof buf - 1, &got, NULL); CloseHandle(h);
    buf[got] = 0;
    return sscanf(buf, "%63s %d %79s", host, port, key) == 3;
}


/* Song length in 100 ns ticks from /Items?Ids=<id> ("RunTimeTicks"), 0 if unknown. */
static LONGLONG fetch_duration(const char *host, int port, const char *key, const char *item) {
    SOCKET s; struct sockaddr_in a; char req[400], *buf, *q; int n, len = 0; LONGLONG t = 0;
    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return 0;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((u_short)port); a.sin_addr.s_addr = inet_addr(host);
    if (connect(s, (struct sockaddr *)&a, sizeof a)) { closesocket(s); return 0; }
    n = _snprintf(req, sizeof req, "GET /Items?Ids=%s&EnableImages=false&EnableUserData=false&api_key=%s HTTP/1.0\r\nHost: %s:%d\r\n\r\n", item, key, host, port);
    send(s, req, n, 0);
    buf = (char *)malloc(65537);
    if (!buf) { closesocket(s); return 0; }
    while (len < 65536 && (n = recv(s, buf + len, 65536 - len, 0)) > 0) len += n;
    closesocket(s);
    buf[len] = 0;
    q = strstr(buf, "\"RunTimeTicks\":");
    if (q) for (q += 15; *q >= '0' && *q <= '9'; q++) t = t * 10 + (*q - '0');
    free(buf);
    slog("load: duration %ld ms", (long)(t / 10000));
    return t;
}

/* Read the first ADTS header (fetching a little more if needed) and set this
   filter's WAVEFORMATEX to the stream's real sample rate and channel count. */
static int read_format(Filter *f) {
    static const int rates[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
    int i, n, sfi, ch, prof, rate;
    while (f->wr < 4096) {                       /* the ring starts at 0 here */
        n = recv(f->sock, (char *)f->ring + f->wr, 4096 - f->wr, 0);
        if (n <= 0) break;
        f->wr += n;
    }
    for (i = 0; i + 7 <= f->wr; i++)
        if (f->ring[i] == 0xFF && (f->ring[i + 1] & 0xF6) == 0xF0) break;
    if (i + 7 > f->wr) { slog("load: no ADTS header in the first %ld bytes", (long)f->wr); return 0; }
    prof = (f->ring[i + 2] >> 6) + 1;
    sfi  = (f->ring[i + 2] >> 2) & 0x0F;
    ch   = ((f->ring[i + 2] & 1) << 2) | (f->ring[i + 3] >> 6);
    if (sfi > 12) { slog("load: bad sample-rate index %d", sfi); return 0; }
    rate = rates[sfi];
    if (ch < 1 || ch > 2) ch = 2;
    f->wfx[2] = (unsigned char)ch; f->wfx[3] = 0;
    f->wfx[4] = (unsigned char)(rate & 0xFF); f->wfx[5] = (unsigned char)((rate >> 8) & 0xFF);
    f->wfx[6] = (unsigned char)((rate >> 16) & 0xFF); f->wfx[7] = 0;
    slog("load: AAC profile %d, %d Hz, %d ch", prof, rate, ch);
    return 1;
}

/* Connect, send the request, read past the headers. Leftover body bytes go into the ring. */
static int open_stream(Filter *f, const char *item, LONGLONG start) {
    char host[64], key[80], req[600], hdr[2048], *e; int port, n, hl = 0, code;
    struct sockaddr_in a; WSADATA w;
    if (!read_config(host, &port, key)) { slog("load: no \\flash2\\zunefin\\config.txt"); return 0; }
    WSAStartup(MAKEWORD(2, 2), &w);              /* refcounted; never WSACleanup in a hosted DLL */
    f->sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (f->sock == INVALID_SOCKET) { slog("load: socket err %d", WSAGetLastError()); return 0; }
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((u_short)port); a.sin_addr.s_addr = inet_addr(host);
    if (connect(f->sock, (struct sockaddr *)&a, sizeof a)) { slog("load: connect %s:%d err %d", host, port, WSAGetLastError()); return 0; }
    if (f->dur == 0) f->dur = fetch_duration(host, port, key, item);
    {   /* start offset as decimal ticks, without 64-bit printf */
        char st[24]; int k = 23; unsigned long long v = (unsigned long long)(start > 0 ? start : 0);
        st[k] = 0; do { st[--k] = (char)('0' + (int)(v % 10)); v /= 10; } while (v && k > 0);
        n = _snprintf(req, sizeof req,
            "GET /Audio/%s/stream.aac?audioCodec=aac&audioBitRate=192000&audioChannels=2&audioSampleRate=44100&maxAudioSampleRate=44100&startTimeTicks=%s&api_key=%s HTTP/1.0\r\n"
            "Host: %s:%d\r\nUser-Agent: ZuneFin-stream/0.1\r\n\r\n", item, st + k, key, host, port);
    }
    send(f->sock, req, n, 0);
    while (hl < (int)sizeof hdr - 1 && (n = recv(f->sock, hdr + hl, sizeof hdr - 1 - hl, 0)) > 0) {
        hl += n; hdr[hl] = 0;
        if ((e = strstr(hdr, "\r\n\r\n")) != NULL) {
            int body = hl - (int)(e + 4 - hdr);
            code = (hl > 12 && !strncmp(hdr, "HTTP/1.", 7)) ? atoi(hdr + 9) : 0;
            if (code != 200) { slog("load: HTTP %d", code); return 0; }
            if (body > 0) { memcpy(f->ring, e + 4, body); f->wr = body; }
            slog("load: stream open, %d body bytes in hand", body);
            return read_format(f);
        }
    }
    slog("load: no HTTP header (got %d bytes)", hl);
    return 0;
}

static DWORD WINAPI fetch_thread(LPVOID param) {
    Filter *f = param; unsigned char tmp[4096]; long total = f->wr;
    for (;;) {
        LONG space = RING - (f->wr - f->rd); int n, want, first, pos;
        if (WaitForSingleObject(f->hStop, 0) == WAIT_OBJECT_0) break;
        if (space < (LONG)sizeof tmp) { Sleep(20); continue; }
        want = sizeof tmp;
        n = recv(f->sock, (char *)tmp, want, 0);
        if (n <= 0) break;
        pos = f->wr % RING; first = RING - pos; if (first > n) first = n;
        memcpy(f->ring + pos, tmp, first);
        if (n > first) memcpy(f->ring, tmp + first, n - first);
        InterlockedExchange((LONG *)&f->wr, f->wr + n);
        total += n;
    }
    slog("fetch: done, %ld bytes", total);
    InterlockedExchange((LONG *)&f->done, 1);
    return 0;
}

/* Copy n bytes at absolute offset `at` out of the ring. */
static void ring_copy(Filter *f, LONG at, unsigned char *dst, int n) {
    int pos = at % RING, first = RING - pos;
    if (first > n) first = n;
    memcpy(dst, f->ring + pos, first);
    if (n > first) memcpy(dst + first, f->ring, n - first);
}
/* Wait until `need` bytes past rd are buffered; 0 = stream ended or stopped. */
static int wait_avail(Filter *f, LONG need) {
    while (f->wr - f->rd < need) {
        if (f->done) return 0;
        if (WaitForSingleObject(f->hStop, 15) == WAIT_OBJECT_0) return 0;
    }
    return 1;
}

static DWORD WINAPI push_thread(LPVOID param) {
    Filter *f = param; Pin *p = f->pin; int frames = 0, resync = 0;
    while (f->wr - f->rd < PREBUF && !f->done)
        if (WaitForSingleObject(f->hStop, 20) == WAIT_OBJECT_0) return 0;
    slog("push: prebuffered %ld bytes", (long)(f->wr - f->rd));
    for (;;) {
        unsigned char h[9]; int flen, hlen; void *samp = NULL; unsigned char *ptr = NULL; HRESULT hr;
        if (WaitForSingleObject(f->hStop, 0) == WAIT_OBJECT_0) break;
        if (!wait_avail(f, 9)) break;
        ring_copy(f, f->rd, h, 9);
        if (h[0] != 0xFF || (h[1] & 0xF6) != 0xF0) {          /* lost sync: skip a byte */
            f->rd++; resync++; continue;
        }
        hlen = (h[1] & 1) ? 7 : 9;
        flen = ((h[3] & 3) << 11) | (h[4] << 3) | (h[5] >> 5);
        if (flen <= hlen || flen > 8192) { f->rd++; resync++; continue; }
        if (!wait_avail(f, flen)) break;
        hr = ((HRESULT (*)(void *, void **, LONGLONG *, LONGLONG *, DWORD))VT(p->alloc)[7])(p->alloc, &samp, NULL, NULL, 0);  /* GetBuffer: paces us */
        if (hr || !samp) { slog("push: GetBuffer 0x%08lx", (unsigned long)hr); break; }
        ((HRESULT (*)(void *, unsigned char **))VT(samp)[3])(samp, &ptr);                         /* GetPointer */
        ring_copy(f, f->rd + hlen, ptr, flen - hlen);
        ((HRESULT (*)(void *, unsigned long))VT(samp)[12])(samp, (unsigned long)(flen - hlen));  /* SetActualDataLength */
        ((HRESULT (*)(void *, LONGLONG *, LONGLONG *))VT(samp)[6])(samp, NULL, NULL);            /* SetTime: untimed */
        ((HRESULT (*)(void *, int))VT(samp)[8])(samp, 1);                                         /* SetSyncPoint */
        if (frames == 0 || InterlockedExchange((LONG *)&f->discont, 0)) ((HRESULT (*)(void *, int))VT(samp)[16])(samp, 1);  /* SetDiscontinuity */
        hr = ((HRESULT (*)(void *, void *))VT(p->mem)[6])(p->mem, samp);                          /* Receive */
        ((ULONG (*)(void *))VT(samp)[2])(samp);
        if (hr) { slog("push: Receive 0x%08lx after %d frames", (unsigned long)hr, frames); break; }
        InterlockedExchange((LONG *)&f->rd, f->rd + flen);
        InterlockedIncrement((LONG *)&f->frames);
        frames++;
        if (frames == 1 || frames % 2000 == 0) slog("push: %d frames", frames);
    }
    slog("push: end after %d frames (%d resync bytes)", frames, resync);
    if (p->peer && WaitForSingleObject(f->hStop, 0) != WAIT_OBJECT_0)
        ((HRESULT (*)(void *))VT(p->peer)[14])(p->peer);                                          /* EndOfStream */
    return 0;
}

/* ---------- IFileSourceFilter ---------- */

static HRESULT F_QI(void *o, const GUID *i, void **out);
static HRESULT FSF_QI(void *o, const GUID *i, void **out) { return F_QI(&from_fsf(o)->vF, i, out); }
static ULONG FSF_AR(void *o) { return InterlockedIncrement(&from_fsf(o)->ref); }
static ULONG FSF_RL(void *o) { return InterlockedDecrement(&from_fsf(o)->ref); }
static HRESULT FSF_Load(void *o, const wchar_t *name, const AMMT *pmt) {
    Filter *f = from_fsf(o); char item[48]; int n = 0; const wchar_t *id = name;
    (void)pmt;
    if (!name) return 0x80070057;
    wcsncpy(f->cur, name, 259); f->cur[259] = 0;
    { const wchar_t *s = wcsstr(name, L"://"); if (s) id = s + 3; }
    while (id[n] && n < 47) {
        wchar_t c = id[n];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) break;
        item[n] = (char)c; n++;
    }
    item[n] = 0;
    slog("load: jf://%s", item);
    if (n < 8) { slog("load: bad id"); return 0x80070057; }
    strcpy(f->item, item);
    f->base = 0; f->frames = 0;
    if (!open_stream(f, item, 0)) {
        if (f->sock != INVALID_SOCKET) { closesocket(f->sock); f->sock = INVALID_SOCKET; }
        return 0x80004005;
    }
    f->hFetch = CreateThread(NULL, 0, fetch_thread, f, 0, NULL);
    return f->hFetch ? 0 : 0x80004005;
}
static HRESULT FSF_GetCurFile(void *o, wchar_t **pp, AMMT *pmt) { (void)pmt; if (pp) *pp = from_fsf(o)->cur; return 0; }

/* ---------- IBaseFilter ---------- */

static HRESULT F_QI(void *o, const GUID *i, void **out) {
    Filter *f = o;
    if (geq(i, &IID_IUnknown_) || geq(i, &IID_IPersist_) || geq(i, &IID_IMediaFilter_) || geq(i, &IID_IBaseFilter_)) { *out = &f->vF; InterlockedIncrement(&f->ref); return 0; }
    if (geq(i, &IID_IFileSourceFilter_)) { *out = &f->vFSF; InterlockedIncrement(&f->ref); return 0; }
    if (geq(i, &IID_IMediaSeeking_)) { *out = &f->vMS; InterlockedIncrement(&f->ref); slog("seek: filter QI(IMediaSeeking)"); return 0; }
    *out = 0; return 0x80004002;
}
static ULONG F_AR(void *o) { return InterlockedIncrement(&((Filter *)o)->ref); }
static void stop_threads(Filter *f) {
    if (f->hStop) SetEvent(f->hStop);
    if (f->sock != INVALID_SOCKET) { closesocket(f->sock); f->sock = INVALID_SOCKET; }   /* unblocks recv */
    if (f->hPush)  { WaitForSingleObject(f->hPush, 5000);  CloseHandle(f->hPush);  f->hPush = 0; }
    if (f->hFetch) { WaitForSingleObject(f->hFetch, 5000); CloseHandle(f->hFetch); f->hFetch = 0; }
}
static ULONG F_RL(void *o) {
    Filter *f = o; LONG r = InterlockedDecrement(&f->ref);
    if (r == 0) {
        stop_threads(f);
        if (f->hStop) CloseHandle(f->hStop);
        DeleteCriticalSection(&f->seek_cs);
        if (f->ring) free(f->ring);
        if (f->pin) LocalFree(f->pin);
        LocalFree(f);
        InterlockedDecrement(&g_objs);
    }
    return r;
}
static HRESULT F_GetClassID(void *o, GUID *id) { (void)o; *id = CLSID_ZFStream; return 0; }
static HRESULT F_Stop(void *o) { Filter *f = o; f->state = 0; slog("filter: stop"); stop_threads(f); return 0; }
static HRESULT F_Pause(void *o) {
    Filter *f = o; f->state = 1;
    if (!f->hPush && f->pin->mem && f->pin->alloc) { f->hPush = CreateThread(NULL, 0, push_thread, f, 0, NULL); slog("filter: pause, push thread started"); }
    return 0;
}
static HRESULT F_Run(void *o, LONGLONG t) { (void)t; ((Filter *)o)->state = 2; return 0; }
static HRESULT F_GetState(void *o, DWORD ms, int *st) { (void)ms; *st = ((Filter *)o)->state; return 0; }
static HRESULT F_SetSyncSource(void *o, void *c) { (void)o; (void)c; return 0; }
static HRESULT F_GetSyncSource(void *o, void **c) { (void)o; *c = 0; return 0; }
static HRESULT F_EnumPins(void *o, void **e) { EnumPins *ep = (EnumPins *)LocalAlloc(LPTR, sizeof(EnumPins)); if (!ep) return 0x8007000e; ep->v = g_EPV; ep->ref = 1; ep->f = o; *e = ep; return 0; }
static HRESULT F_FindPin(void *o, const wchar_t *id, void **pp) { (void)o; (void)id; *pp = 0; return 0x80004005; }
static HRESULT F_QueryFilterInfo(void *o, FILTERINFO *info) { Filter *f = o; wcscpy(info->achName, L"ZuneFin Stream"); info->pGraph = f->graph; if (f->graph) ((ULONG (*)(void *))VT(f->graph)[1])(f->graph); return 0; }
static HRESULT F_JoinFilterGraph(void *o, void *g, const wchar_t *n) { (void)n; ((Filter *)o)->graph = g; return 0; }
static HRESULT F_QueryVendorInfo(void *o, wchar_t **v) { (void)o; *v = 0; return 0x80004001; }


/* ---------- IMediaSeeking ---------- */

#define AM_SEEKING_CAPS 0x3F   /* absolute, forwards, backwards, current pos, stop pos, duration */
static Filter *from_ms(void *o) { return (Filter *)((char *)o - offsetof(Filter, vMS)); }

static LONGLONG cur_pos(Filter *f) {
    int rate = f->wfx[4] | (f->wfx[5] << 8) | (f->wfx[6] << 16);
    LONGLONG p;
    if (rate <= 0) rate = 44100;
    p = f->base + (LONGLONG)f->frames * 1024 * 10000000 / rate;    /* 1024 samples per AAC frame */
    return (f->dur && p > f->dur) ? f->dur : p;
}

/* Jump to `pos`: flush downstream, stop the threads, reopen the stream there, restart. */
static HRESULT do_seek(Filter *f, LONGLONG pos) {
    Pin *p = f->pin; int was_running;
    if (pos < 0) pos = 0;
    if (f->dur && pos > f->dur - 10000000) pos = f->dur > 10000000 ? f->dur - 10000000 : 0;   /* keep 1 s */
    EnterCriticalSection(&f->seek_cs);
    slog("seek: to %ld ms (state %d)", (long)(pos / 10000), f->state);
    was_running = f->hPush != 0;
    if (p->peer) ((HRESULT (*)(void *))VT(p->peer)[15])(p->peer);         /* BeginFlush: unblocks GetBuffer/Receive */
    stop_threads(f);
    if (p->peer) ((HRESULT (*)(void *))VT(p->peer)[16])(p->peer);         /* EndFlush */
    ResetEvent(f->hStop);
    f->wr = 0; f->rd = 0; f->done = 0;
    f->base = pos; f->frames = 0; f->discont = 1;
    if (!open_stream(f, f->item, pos)) {
        slog("seek: reopen failed");
        if (f->sock != INVALID_SOCKET) { closesocket(f->sock); f->sock = INVALID_SOCKET; }
        LeaveCriticalSection(&f->seek_cs);
        return 0x80004005;
    }
    f->hFetch = CreateThread(NULL, 0, fetch_thread, f, 0, NULL);
    if (was_running || f->state != 0) f->hPush = CreateThread(NULL, 0, push_thread, f, 0, NULL);
    LeaveCriticalSection(&f->seek_cs);
    return 0;
}

static HRESULT MS_QI(void *o, const GUID *i, void **out) { return F_QI(&from_ms(o)->vF, i, out); }
static ULONG MS_AR(void *o) { return InterlockedIncrement(&from_ms(o)->ref); }
static ULONG MS_RL(void *o) { return F_RL(&from_ms(o)->vF); }
static HRESULT MS_GetCapabilities(void *o, DWORD *c) { (void)o; *c = AM_SEEKING_CAPS; return 0; }
static HRESULT MS_CheckCapabilities(void *o, DWORD *c) { (void)o; *c &= AM_SEEKING_CAPS; return *c ? 0 : 1; }
static HRESULT MS_IsFormatSupported(void *o, const GUID *g) { (void)o; return geq(g, &TIME_FORMAT_MEDIA_TIME_) ? 0 : 1; }
static HRESULT MS_QueryPreferredFormat(void *o, GUID *g) { (void)o; *g = TIME_FORMAT_MEDIA_TIME_; return 0; }
static HRESULT MS_GetTimeFormat(void *o, GUID *g) { (void)o; *g = TIME_FORMAT_MEDIA_TIME_; return 0; }
static HRESULT MS_IsUsingTimeFormat(void *o, const GUID *g) { (void)o; return geq(g, &TIME_FORMAT_MEDIA_TIME_) ? 0 : 1; }
static HRESULT MS_SetTimeFormat(void *o, const GUID *g) { (void)o; return geq(g, &TIME_FORMAT_MEDIA_TIME_) ? 0 : 0x80004001; }
static HRESULT MS_GetDuration(void *o, LONGLONG *d) { Filter *f = from_ms(o); if (!f->dur) return 0x80004005; *d = f->dur; return 0; }
static HRESULT MS_GetStopPosition(void *o, LONGLONG *s) { *s = from_ms(o)->dur; return 0; }
static HRESULT MS_GetCurrentPosition(void *o, LONGLONG *c) { *c = cur_pos(from_ms(o)); return 0; }
static HRESULT MS_ConvertTimeFormat(void *o, LONGLONG *t, const GUID *tf, LONGLONG s, const GUID *sf) { (void)o; (void)tf; (void)sf; *t = s; return 0; }
static HRESULT MS_SetPositions(void *o, LONGLONG *cur, DWORD cf, LONGLONG *stop, DWORD sf) {
    Filter *f = from_ms(o); DWORD how = cf & 3; LONGLONG target;
    (void)stop; (void)sf;
    slog("seek: SetPositions flags 0x%lx", (unsigned long)cf);
    if (how == 0 || !cur) return 0;                       /* no change to the current position */
    target = (how == 1) ? *cur : cur_pos(f) + *cur;       /* absolute, or relative/incremental */
    return do_seek(f, target);
}
static HRESULT MS_GetPositions(void *o, LONGLONG *c, LONGLONG *s) { Filter *f = from_ms(o); if (c) *c = cur_pos(f); if (s) *s = f->dur; return 0; }
static HRESULT MS_GetAvailable(void *o, LONGLONG *e, LONGLONG *l) { Filter *f = from_ms(o); if (e) *e = 0; if (l) *l = f->dur; return 0; }
static HRESULT MS_SetRate(void *o, double r) { (void)o; return r == 1.0 ? 0 : 0x80004001; }
static HRESULT MS_GetRate(void *o, double *r) { (void)o; *r = 1.0; return 0; }
static HRESULT MS_GetPreroll(void *o, LONGLONG *p) { (void)o; *p = 0; return 0; }

static Filter *make_filter(void) {
    Filter *f = (Filter *)LocalAlloc(LPTR, sizeof(Filter));
    Pin *p = (Pin *)LocalAlloc(LPTR, sizeof(Pin));
    unsigned char *ring = (unsigned char *)malloc(RING);
    if (!f || !p || !ring) { if (f) LocalFree(f); if (p) LocalFree(p); if (ring) free(ring); return NULL; }
    f->vF = g_FV; f->vFSF = g_SV; f->vMS = g_MSV; f->ref = 1; f->pin = p; f->sock = INVALID_SOCKET; f->ring = ring;
    f->hStop = CreateEventW(NULL, TRUE, FALSE, NULL);
    InitializeCriticalSection(&f->seek_cs);
    memcpy(f->wfx, g_wfx_default, 18);
    p->v = g_PV; p->ref = 1; p->f = f; fill_mt(&p->mt, f->wfx);
    InterlockedIncrement(&g_objs);
    return f;
}

/* ---------- IClassFactory ---------- */

static HRESULT CF_QI(void *o, const GUID *i, void **out) { if (geq(i, &IID_IUnknown_) || geq(i, &IID_IClassFactory_)) { *out = o; return 0; } *out = 0; return 0x80004002; }
static ULONG CF_AR(void *o) { return InterlockedIncrement(&((Factory *)o)->ref); }
static ULONG CF_RL(void *o) { return InterlockedDecrement(&((Factory *)o)->ref); }
static HRESULT CF_CreateInstance(void *o, void *outer, const GUID *iid, void **ppv) {
    Filter *f; HRESULT hr; (void)o;
    if (outer) { *ppv = 0; return 0x80040110; }
    f = make_filter();
    if (!f) { *ppv = 0; slog("factory: out of memory"); return 0x8007000e; }
    hr = F_QI(&f->vF, iid, ppv);
    F_RL(&f->vF);
    slog("factory: created filter 0x%08lx", (unsigned long)hr);
    return hr;
}
static HRESULT CF_LockServer(void *o, BOOL l) { (void)o; (void)l; return 0; }

static void init_vtbls(void) {
    void *F[15]  = {F_QI, F_AR, F_RL, F_GetClassID, F_Stop, F_Pause, F_Run, F_GetState, F_SetSyncSource, F_GetSyncSource, F_EnumPins, F_FindPin, F_QueryFilterInfo, F_JoinFilterGraph, F_QueryVendorInfo};
    void *S[5]   = {FSF_QI, FSF_AR, FSF_RL, FSF_Load, FSF_GetCurFile};
    void *P[18]  = {P_QI, P_AR, P_RL, P_Connect, P_ReceiveConnection, P_Disconnect, P_ConnectedTo, P_ConnectionMediaType, P_QueryPinInfo, P_QueryDirection, P_QueryId, P_QueryAccept, P_EnumMediaTypes, P_QueryInternalConnections, P_EndOfStream, P_BeginFlush, P_EndFlush, P_NewSegment};
    void *EP[7]  = {EP_QI, EP_AR, EP_RL, EP_Next, EP_Skip, EP_Reset, EP_Clone};
    void *EM[7]  = {EM_QI, EM_AR, EM_RL, EM_Next, EM_Skip, EM_Reset, EM_Clone};
    void *CF[5]  = {CF_QI, CF_AR, CF_RL, CF_CreateInstance, CF_LockServer};
    void *MS[20] = {MS_QI, MS_AR, MS_RL, MS_GetCapabilities, MS_CheckCapabilities, MS_IsFormatSupported, MS_QueryPreferredFormat,
                    MS_GetTimeFormat, MS_IsUsingTimeFormat, MS_SetTimeFormat, MS_GetDuration, MS_GetStopPosition, MS_GetCurrentPosition,
                    MS_ConvertTimeFormat, MS_SetPositions, MS_GetPositions, MS_GetAvailable, MS_SetRate, MS_GetRate, MS_GetPreroll};
    memcpy(g_FV, F, sizeof F); memcpy(g_SV, S, sizeof S); memcpy(g_PV, P, sizeof P);
    memcpy(g_EPV, EP, sizeof EP); memcpy(g_EMV, EM, sizeof EM); memcpy(g_CFV, CF, sizeof CF); memcpy(g_MSV, MS, sizeof MS);
    g_factory.v = g_CFV; g_factory.ref = 1;
}

/* ---------- COM server exports ---------- */

__declspec(dllexport) HRESULT DllGetClassObject(const GUID *clsid, const GUID *iid, void **ppv) {
    if (geq(clsid, &CLSID_ZFStream) && (geq(iid, &IID_IClassFactory_) || geq(iid, &IID_IUnknown_))) { *ppv = &g_factory; return 0; }
    *ppv = 0; return 0x80040111;
}
__declspec(dllexport) HRESULT DllCanUnloadNow(void) { return g_objs > 0 ? 1 : 0; }

BOOL WINAPI DllMain(HANDLE h, DWORD reason, LPVOID r) {
    (void)h; (void)r;
    if (reason == DLL_PROCESS_ATTACH) { init_vtbls(); slog("dll: loaded"); }
    return TRUE;
}
