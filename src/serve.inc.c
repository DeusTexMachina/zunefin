/* serve.inc.c: ZuneFin's on-device browse server (included by jfplay.c in JF_EXE builds).
 *
 * Serves simple HTML pages on port 8090 for the Zune's own browser
 * (http://127.0.0.1:8090/): artists, albums, tracks, search. Tapping Play starts
 * album playback on a worker thread through the native queue (album_play()).
 * Cover thumbnails are proxied from Jellyfin so the browser never sees the API key.
 */

#define SERVE_PORT 8090
#define PAGE_SIZE  40

static char g_host[64], g_key[80];
static int g_port = 8096;

/* ---------- growable output buffer ---------- */

typedef struct { char *p; int len, cap; } Buf;

static void b_put(Buf *b, const char *s, int n) {
    if (n < 0) n = (int)strlen(s);
    if (b->len + n + 1 > b->cap) {
        int nc = b->cap ? b->cap * 2 : 8192; char *np;
        while (nc < b->len + n + 1) nc *= 2;
        np = (char *)realloc(b->p, nc);
        if (!np) return;
        b->p = np; b->cap = nc;
    }
    memcpy(b->p + b->len, s, n); b->len += n; b->p[b->len] = 0;
}
static void b_printf(Buf *b, const char *fmt, ...) {
    char tmp[1024]; va_list ap; int n;
    va_start(ap, fmt); n = _vsnprintf(tmp, sizeof tmp - 1, fmt, ap); va_end(ap);
    if (n < 0) n = sizeof tmp - 1;
    tmp[n] = 0;
    b_put(b, tmp, n);
}
/* HTML-escape a UTF-8 string */
static void b_esc(Buf *b, const char *s) {
    for (; *s; s++) {
        switch (*s) {
        case '<': b_put(b, "&lt;", 4); break;
        case '>': b_put(b, "&gt;", 4); break;
        case '&': b_put(b, "&amp;", 5); break;
        case '"': b_put(b, "&quot;", 6); break;
        default:  b_put(b, s, 1);
        }
    }
}

/* ---------- small URL helpers ---------- */

static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
/* Get query parameter `name` from "a=1&b=2", URL-decoded. */
static int qparam(const char *q, const char *name, char *out, int cap) {
    int nl = (int)strlen(name), i = 0;
    const char *p = q;
    out[0] = 0;
    while (p && *p) {
        if (!strncmp(p, name, nl) && p[nl] == '=') {
            p += nl + 1;
            while (*p && *p != '&' && i < cap - 1) {
                if (*p == '+') { out[i++] = ' '; p++; }
                else if (*p == '%' && hexv(p[1]) >= 0 && hexv(p[2]) >= 0) { out[i++] = (char)(hexv(p[1]) * 16 + hexv(p[2])); p += 3; }
                else out[i++] = *p++;
            }
            out[i] = 0;
            return 1;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return 0;
}
/* Percent-encode for a URL query value. */
static void urlenc(const char *s, char *out, int cap) {
    static const char *H = "0123456789ABCDEF"; int i = 0;
    for (; *s && i < cap - 4; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')
            out[i++] = (char)c;
        else { out[i++] = '%'; out[i++] = H[c >> 4]; out[i++] = H[c & 15]; }
    }
    out[i] = 0;
}
/* Only allow hex ids into Jellyfin URLs. */
static int valid_id(const char *s) {
    int n = 0;
    for (; *s; s++, n++) if (hexv(*s) < 0) return 0;
    return n >= 8 && n <= 40;
}

/* ---------- page chrome ---------- */

static void page_head(Buf *b, const char *title) {
    b_put(b, "<html><head><meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\">"
             "<meta name=\"viewport\" content=\"width=device-width\"><meta name=\"MobileOptimized\" content=\"272\">"
             "<title>", -1);
    b_esc(b, title);
    b_put(b, "</title><style>"
             "body{background:#000;color:#fff;font-family:Segoe UI,Tahoma,sans-serif;margin:0;padding:6px}"
             "a{color:#fff;text-decoration:none}"
             "h1{font-size:28px;font-weight:normal;margin:4px 0 8px 0;color:#f4f4f4}"
             ".r{display:block;padding:9px 4px;border-bottom:1px solid #333;font-size:18px}"
             ".s{color:#999;font-size:14px}"
             ".n{color:#e6007e}"
             ".bar{font-size:14px;padding:6px 0;color:#999}"
             ".btn{display:block;background:#e6007e;color:#fff;padding:10px;margin:8px 0;text-align:center;font-size:18px}"
             ".t{width:56px;height:56px;vertical-align:middle;margin-right:8px;border:0}"
             ".az a{display:inline-block;padding:6px 7px;font-size:16px;color:#e6007e}"
             "</style></head><body>", -1);
    b_put(b, "<div class=\"bar\"><a href=\"/\">zunefin</a> &middot; ", -1);
    b_esc(b, g_status);
    b_put(b, "</div><h1>", -1);
    b_esc(b, title);
    b_put(b, "</h1>", -1);
}
static void page_foot(Buf *b) {
    b_put(b, "<div class=\"bar\"><a href=\"/\">home</a> &middot; <a href=\"/artists\">artists</a> &middot; "
             "<a href=\"/albums\">albums</a> &middot; <a href=\"/search\">search</a></div></body></html>", -1);
}
static void az_bar(Buf *b, const char *base) {
    const char *L = "#ABCDEFGHIJKLMNOPQRSTUVWXYZ"; int i;
    b_put(b, "<div class=\"az\">", -1);
    for (i = 0; L[i]; i++) {
        if (L[i] == '#') b_printf(b, "<a href=\"%s\">all</a>", base);
        else b_printf(b, "<a href=\"%s?l=%c\">%c</a>", base, L[i], L[i]);
    }
    b_put(b, "</div>", -1);
}

/* ---------- Jellyfin helpers ---------- */

static char *jf_get(const char *fmt, ...) {
    char path[700]; va_list ap; int st; char *body; int n;
    va_start(ap, fmt); n = _vsnprintf(path, sizeof path - 100, fmt, ap); va_end(ap);
    if (n < 0) return NULL;
    _snprintf(path + n, 100, "%sapi_key=%s", strchr(path, '?') ? "&" : "?", g_key);
    body = http_get(g_host, g_port, path, &st);
    if (body && st != 200) { flog("serve: HTTP %d for %.80s", st, path); free(body); return NULL; }
    return body;
}
static int json_int(const char *p, const char *key) {
    char pat[40]; const char *q;
    _snprintf(pat, sizeof pat, "\"%s\":", key);
    q = strstr(p, pat);
    return q ? atoi(q + strlen(pat)) : 0;
}

/* ---------- pages ---------- */

static void pg_home(Buf *b) {
    page_head(b, "jellyfin");
    b_put(b, "<a class=\"r\" href=\"/artists\">artists</a>"
             "<a class=\"r\" href=\"/albums\">albums</a>"
             "<a class=\"r\" href=\"/search\">search</a>"
             "<a class=\"r\" href=\"/play?album=random\">shuffle a random album</a>", -1);
    page_foot(b);
}

static void pg_artists(Buf *b, const char *q) {
    char l[4], st[12], *body; int start; const char *p; char *obj; int total;
    qparam(q, "l", l, sizeof l); qparam(q, "s", st, sizeof st); start = atoi(st);
    if (l[0]) body = jf_get("/Artists/AlbumArtists?SortBy=SortName&NameStartsWith=%c&StartIndex=%d&Limit=%d&EnableImages=false&EnableUserData=false", l[0], start, PAGE_SIZE);
    else      body = jf_get("/Artists/AlbumArtists?SortBy=SortName&StartIndex=%d&Limit=%d&EnableImages=false&EnableUserData=false", start, PAGE_SIZE);
    page_head(b, "artists");
    az_bar(b, "/artists");
    if (!body) { b_put(b, "<p>Couldn't reach Jellyfin.</p>", -1); page_foot(b); return; }
    total = json_int(body, "TotalRecordCount");
    p = strstr(body, "\"Items\":[");
    if (p) p += 9;
    while (p && (obj = json_next_obj(&p)) != NULL) {
        char id[48], name[160];
        if (json_str(obj, "Id", id, sizeof id) && json_str(obj, "Name", name, sizeof name)) {
            b_printf(b, "<a class=\"r\" href=\"/artist?id=%s\">", id); b_esc(b, name); b_put(b, "</a>", -1);
        }
        free(obj);
    }
    if (start + PAGE_SIZE < total)
        b_printf(b, "<a class=\"btn\" href=\"/artists?l=%s&s=%d\">more (%d of %d)</a>", l, start + PAGE_SIZE, start + PAGE_SIZE, total);
    free(body);
    page_foot(b);
}

/* list of albums from an Items reply */
static void album_rows(Buf *b, const char *body) {
    const char *p = strstr(body, "\"Items\":["); char *obj;
    if (p) p += 9;
    while (p && (obj = json_next_obj(&p)) != NULL) {
        char id[48], name[160], artist[160]; int year = json_int(obj, "ProductionYear");
        artist[0] = 0;
        if (json_str(obj, "Id", id, sizeof id) && json_str(obj, "Name", name, sizeof name)) {
            json_str(obj, "AlbumArtist", artist, sizeof artist);
            b_printf(b, "<a class=\"r\" href=\"/album?id=%s\"><img class=\"t\" src=\"/img?id=%s\">", id, id);
            b_esc(b, name);
            b_put(b, "<br><span class=\"s\">", -1); b_esc(b, artist);
            if (year) b_printf(b, " &middot; %d", year);
            b_put(b, "</span></a>", -1);
        }
        free(obj);
    }
}

static void pg_albums(Buf *b, const char *q) {
    char l[4], st[12], *body; int start, total;
    qparam(q, "l", l, sizeof l); qparam(q, "s", st, sizeof st); start = atoi(st);
    if (l[0]) body = jf_get("/Items?IncludeItemTypes=MusicAlbum&Recursive=true&SortBy=SortName&NameStartsWith=%c&StartIndex=%d&Limit=%d&EnableImages=false&EnableUserData=false", l[0], start, PAGE_SIZE);
    else      body = jf_get("/Items?IncludeItemTypes=MusicAlbum&Recursive=true&SortBy=SortName&StartIndex=%d&Limit=%d&EnableImages=false&EnableUserData=false", start, PAGE_SIZE);
    page_head(b, "albums");
    az_bar(b, "/albums");
    if (!body) { b_put(b, "<p>Couldn't reach Jellyfin.</p>", -1); page_foot(b); return; }
    total = json_int(body, "TotalRecordCount");
    album_rows(b, body);
    if (start + PAGE_SIZE < total)
        b_printf(b, "<a class=\"btn\" href=\"/albums?l=%s&s=%d\">more (%d of %d)</a>", l, start + PAGE_SIZE, start + PAGE_SIZE, total);
    free(body);
    page_foot(b);
}

static void pg_artist(Buf *b, const char *q) {
    char id[48], name[160] = "artist", *body;
    qparam(q, "id", id, sizeof id);
    if (!valid_id(id)) { page_head(b, "artist"); b_put(b, "<p>Bad link.</p>", -1); page_foot(b); return; }
    body = jf_get("/Items?Ids=%s&EnableImages=false&EnableUserData=false", id);
    if (body) { const char *it = strstr(body, "\"Items\":["); if (it) json_str(it, "Name", name, sizeof name); free(body); }
    body = jf_get("/Items?IncludeItemTypes=MusicAlbum&Recursive=true&AlbumArtistIds=%s&SortBy=ProductionYear,SortName&EnableImages=false&EnableUserData=false", id);
    page_head(b, name);
    if (!body) { b_put(b, "<p>Couldn't reach Jellyfin.</p>", -1); page_foot(b); return; }
    album_rows(b, body);
    free(body);
    page_foot(b);
}

static void pg_album(Buf *b, const char *q) {
    char id[48], name[160] = "album", artist[160] = "", *body; Track *tr; int n, k, year = 0;
    qparam(q, "id", id, sizeof id);
    if (!valid_id(id)) { page_head(b, "album"); b_put(b, "<p>Bad link.</p>", -1); page_foot(b); return; }
    body = jf_get("/Items?Ids=%s&EnableImages=false&EnableUserData=false", id);
    if (body) {
        const char *it = strstr(body, "\"Items\":[");
        if (it) { json_str(it, "Name", name, sizeof name); json_str(it, "AlbumArtist", artist, sizeof artist); year = json_int(it, "ProductionYear"); }
        free(body);
    }
    page_head(b, name);
    b_printf(b, "<img src=\"/img?id=%s&big=1\" width=\"120\" height=\"120\" align=\"left\" style=\"margin:0 8px 8px 0\">", id);
    b_put(b, "<div class=\"s\">", -1); b_esc(b, artist);
    if (year) b_printf(b, "<br>%d", year);
    b_put(b, "</div><br clear=\"all\">", -1);
    b_printf(b, "<a class=\"btn\" href=\"/play?album=%s\">play album</a>", id);
    body = jf_get("/Items?ParentId=%s&IncludeItemTypes=Audio&Recursive=true&SortBy=ParentIndexNumber,IndexNumber,SortName&EnableImages=false&EnableUserData=false&Limit=%d", id, MAX_TRACKS);
    tr = (Track *)malloc(sizeof(Track) * MAX_TRACKS);
    if (!body || !tr) { b_put(b, "<p>Couldn't reach Jellyfin.</p>", -1); free(body); free(tr); page_foot(b); return; }
    n = parse_tracks_into(body, tr, MAX_TRACKS);
    free(body);
    for (k = 0; k < n; k++) {
        b_printf(b, "<a class=\"r\" href=\"/play?album=%s&track=%s\"><span class=\"n\">%d</span> ", id, tr[k].id, k + 1);
        b_esc(b, tr[k].name);
        b_printf(b, " <span class=\"s\">%d:%02d</span></a>", tr[k].dur_ms / 60000, (tr[k].dur_ms / 1000) % 60);
    }
    free(tr);
    page_foot(b);
}

static void pg_search(Buf *b, const char *q) {
    char term[120], enc[360], *body; const char *p; char *obj;
    qparam(q, "q", term, sizeof term);
    page_head(b, "search");
    b_put(b, "<form action=\"/search\" method=\"get\" accept-charset=\"utf-8\">"
             "<input name=\"q\" style=\"width:70%;font-size:18px\" value=\"", -1);
    b_esc(b, term);
    b_put(b, "\"> <input type=\"submit\" value=\"go\" style=\"font-size:18px\"></form>", -1);
    if (!term[0]) { page_foot(b); return; }
    urlenc(term, enc, sizeof enc);
    body = jf_get("/Items?SearchTerm=%s&IncludeItemTypes=MusicArtist,MusicAlbum,Audio&Recursive=true&Limit=40&EnableImages=false&EnableUserData=false", enc);
    if (!body) { b_put(b, "<p>Couldn't reach Jellyfin.</p>", -1); page_foot(b); return; }
    p = strstr(body, "\"Items\":[");
    if (p) p += 9;
    while (p && (obj = json_next_obj(&p)) != NULL) {
        char id[48], name[160], type[24], extra[160], aid[48];
        extra[0] = aid[0] = 0;
        if (json_str(obj, "Id", id, sizeof id) && json_str(obj, "Name", name, sizeof name) && json_str(obj, "Type", type, sizeof type)) {
            if (!strcmp(type, "MusicArtist")) {
                b_printf(b, "<a class=\"r\" href=\"/artist?id=%s\">", id); b_esc(b, name);
                b_put(b, " <span class=\"s\">artist</span></a>", -1);
            } else if (!strcmp(type, "MusicAlbum")) {
                json_str(obj, "AlbumArtist", extra, sizeof extra);
                b_printf(b, "<a class=\"r\" href=\"/album?id=%s\">", id); b_esc(b, name);
                b_put(b, " <span class=\"s\">album &middot; ", -1); b_esc(b, extra); b_put(b, "</span></a>", -1);
            } else if (!strcmp(type, "Audio") && json_str(obj, "AlbumId", aid, sizeof aid)) {
                json_str(obj, "Album", extra, sizeof extra);
                b_printf(b, "<a class=\"r\" href=\"/play?album=%s&track=%s\">", aid, id); b_esc(b, name);
                b_put(b, " <span class=\"s\">song &middot; ", -1); b_esc(b, extra); b_put(b, "</span></a>", -1);
            }
        }
        free(obj);
    }
    free(body);
    page_foot(b);
}

/* ---------- playback worker ---------- */

static HANDLE g_worker;
static char g_req_album[48], g_req_track[48];

static DWORD WINAPI play_worker(LPVOID unused) {
    static char outbuf[4096]; int used = 0;
    (void)unused;
    o_buf = outbuf; o_max = sizeof outbuf; o_used = 0; outbuf[0] = 0;   /* say() target for this run */
    flog("serve: play album=%s track=%s", g_req_album, g_req_track);
    album_play(g_host, g_port, g_key, g_req_album, g_req_track);
    used = o_used; (void)used;
    flush_out();
    return 0;
}

static void start_playback(const char *album, const char *track) {
    if (g_worker) {                         /* cancel the current album first */
        SetEvent(g_play_stop);
        WaitForSingleObject(g_worker, 20000);
        CloseHandle(g_worker); g_worker = NULL;
    }
    ResetEvent(g_play_stop);
    strncpy(g_req_album, album, sizeof g_req_album - 1); g_req_album[sizeof g_req_album - 1] = 0;
    strncpy(g_req_track, track, sizeof g_req_track - 1); g_req_track[sizeof g_req_track - 1] = 0;
    _snprintf(g_status, sizeof g_status, "Starting... first song in about 40 s");
    g_worker = CreateThread(NULL, 0, play_worker, NULL, 0, NULL);
}

static void pg_play(Buf *b, const char *q) {
    char album[48], track[48];
    qparam(q, "album", album, sizeof album); qparam(q, "track", track, sizeof track);
    if (strcmp(album, "random") != 0 && !valid_id(album)) { page_head(b, "play"); b_put(b, "<p>Bad link.</p>", -1); page_foot(b); return; }
    if (track[0] && !valid_id(track)) track[0] = 0;
    start_playback(album, track);
    page_head(b, "starting");
    b_put(b, "<p>Downloading the first song. It will start playing in about 40 seconds. "
             "The rest of the album follows in the Zune's Now Playing queue.</p>", -1);
    if (strcmp(album, "random") != 0) b_printf(b, "<a class=\"btn\" href=\"/album?id=%s\">back to the album</a>", album);
    page_foot(b);
}

/* ---------- HTTP plumbing ---------- */

static void send_all(SOCKET c, const char *p, int n) {
    while (n > 0) { int k = send(c, p, n, 0); if (k <= 0) return; p += k; n -= k; }
}
static void send_page(SOCKET c, int code, Buf *b) {
    char h[200];
    int n = _snprintf(h, sizeof h, "HTTP/1.0 %d OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                      "Cache-Control: no-cache\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", code, b->len);
    send_all(c, h, n);
    send_all(c, b->p ? b->p : "", b->len);
}

/* Relay a cover thumbnail from Jellyfin. */
static void relay_img(SOCKET c, const char *q) {
    char id[48], big[4], path[300], req[400], buf[4096]; SOCKET s; struct sockaddr_in a; int n, hdr = 0, size;
    qparam(q, "id", id, sizeof id); qparam(q, "big", big, sizeof big);
    if (!valid_id(id)) { send_all(c, "HTTP/1.0 404 Not Found\r\n\r\n", 26); return; }
    size = big[0] ? 120 : 56;
    _snprintf(path, sizeof path, "/Items/%s/Images/Primary?maxWidth=%d&maxHeight=%d&format=Jpg&quality=80&api_key=%s", id, size, size, g_key);
    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((u_short)g_port); a.sin_addr.s_addr = inet_addr(g_host);
    if (s == INVALID_SOCKET || connect(s, (struct sockaddr *)&a, sizeof a)) {
        if (s != INVALID_SOCKET) closesocket(s);
        send_all(c, "HTTP/1.0 502 Bad Gateway\r\n\r\n", 28); return;
    }
    n = _snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: %s:%d\r\n\r\n", path, g_host, g_port);
    send(s, req, n, 0);
    /* pass the status line through, replace headers with our own cacheable ones */
    {
        int hl = 0; char *e;
        while (!hdr && (n = recv(s, buf + hl, sizeof buf - 1 - hl, 0)) > 0) {
            hl += n; buf[hl] = 0;
            if ((e = strstr(buf, "\r\n\r\n")) != NULL) {
                int code = (hl > 12 && !strncmp(buf, "HTTP/1.", 7)) ? atoi(buf + 9) : 502;
                char h[160]; int m;
                hdr = 1;
                if (code != 200) { send_all(c, "HTTP/1.0 404 Not Found\r\n\r\n", 26); closesocket(s); return; }
                m = _snprintf(h, sizeof h, "HTTP/1.0 200 OK\r\nContent-Type: image/jpeg\r\nCache-Control: max-age=86400\r\nConnection: close\r\n\r\n");
                send_all(c, h, m);
                send_all(c, e + 4, hl - (int)(e + 4 - buf));
            } else if (hl >= (int)sizeof buf - 1) break;
        }
    }
    while (hdr && (n = recv(s, buf, sizeof buf, 0)) > 0) send_all(c, buf, n);
    closesocket(s);
}

static int handle(SOCKET c) {
    char req[2048], path[600], *sp, *q; int n, len = 0; Buf b = {0, 0, 0};
    while (len < (int)sizeof req - 1 && (n = recv(c, req + len, sizeof req - 1 - len, 0)) > 0) {
        len += n; req[len] = 0;
        if (strstr(req, "\r\n\r\n")) break;
    }
    if (len < 5 || strncmp(req, "GET ", 4) != 0) return 0;
    sp = strchr(req + 4, ' ');
    if (!sp) return 0;
    n = (int)(sp - (req + 4)); if (n >= (int)sizeof path) n = sizeof path - 1;
    memcpy(path, req + 4, n); path[n] = 0;
    q = strchr(path, '?');
    if (q) *q++ = 0; else q = path + strlen(path);

    if (!strcmp(path, "/img"))            { relay_img(c, q); return 0; }
    if (!strcmp(path, "/quit"))           { b_put(&b, "bye", 3); send_page(c, 200, &b); free(b.p); return 1; }
    if (!strcmp(path, "/status"))         { b_put(&b, g_status, -1); send_page(c, 200, &b); free(b.p); return 0; }

    if      (!strcmp(path, "/"))          pg_home(&b);
    else if (!strcmp(path, "/artists"))   pg_artists(&b, q);
    else if (!strcmp(path, "/artist"))    pg_artist(&b, q);
    else if (!strcmp(path, "/albums"))    pg_albums(&b, q);
    else if (!strcmp(path, "/album"))     pg_album(&b, q);
    else if (!strcmp(path, "/search"))    pg_search(&b, q);
    else if (!strcmp(path, "/play"))      pg_play(&b, q);
    else { b_put(&b, "<html><body>not found</body></html>", -1); send_page(c, 404, &b); free(b.p); return 0; }
    send_page(c, 200, &b);
    free(b.p);
    return 0;
}

/* ---------- config + main loop ---------- */

#define CONFIG_FILE L"\\flash2\\zunefin\\config.txt"

static int load_config(void) {
    HANDLE h = CreateFileW(CONFIG_FILE, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    char buf[300]; DWORD got = 0;
    if (h == INVALID_HANDLE_VALUE) return 0;
    ReadFile(h, buf, sizeof buf - 1, &got, NULL); CloseHandle(h);
    buf[got] = 0;
    return sscanf(buf, "%63s %d %79s", g_host, &g_port, g_key) == 3;
}
static void save_config(void) {
    HANDLE h; char buf[300]; DWORD w; int n;
    CreateDirectoryW(L"\\flash2\\zunefin", NULL);
    h = CreateFileW(CONFIG_FILE, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    n = _snprintf(buf, sizeof buf, "%s %d %s\r\n", g_host, g_port, g_key);
    WriteFile(h, buf, n, &w, NULL); CloseHandle(h);
}

static void serve_main(void) {
    SOCKET ls; struct sockaddr_in a; int on = 1;
    g_play_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof on);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(SERVE_PORT); a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (ls == INVALID_SOCKET || bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 8)) {
        flog("serve: can't listen on %d (err %d)", SERVE_PORT, WSAGetLastError());
        return;
    }
    flog("serve: listening on %d, jellyfin %s:%d", SERVE_PORT, g_host, g_port);
    for (;;) {
        fd_set rs; struct timeval tv; SOCKET c;
        if (g_stop && WaitForSingleObject(g_stop, 0) == WAIT_OBJECT_0) break;
        FD_ZERO(&rs); FD_SET(ls, &rs); tv.tv_sec = 1; tv.tv_usec = 0;
        if (select(0, &rs, NULL, NULL, &tv) <= 0) continue;
        c = accept(ls, NULL, NULL);
        if (c == INVALID_SOCKET) continue;
        if (handle(c)) { closesocket(c); break; }
        closesocket(c);
    }
    closesocket(ls);
    if (g_worker) { SetEvent(g_play_stop); WaitForSingleObject(g_worker, 20000); }
    flog("serve: stopped");
}
