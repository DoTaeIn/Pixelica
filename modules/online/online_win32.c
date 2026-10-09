/* Windows platform layer for the online module: SRW lock + condition variable,
 * a _beginthreadex worker, GetTickCount64 and a native WinHTTP transport.
 *
 * TLS uses the system certificate store and host name validation; no
 * SECURITY_FLAG_IGNORE_* flag is ever set. Redirects are disabled and automatic
 * Windows credential logon is restricted to "never". WinHTTP timeouts are
 * per phase (resolve, connect, send, receive), so the whole request is capped
 * only on a best-effort basis: the body read loop additionally stops once
 * timeout_ms has elapsed since the request was sent. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#include <process.h>

#include <stdlib.h>
#include <string.h>

#include "online_internal.h"

struct PIXELICA_OnlineSync {
    SRWLOCK lock;
    CONDITION_VARIABLE cond;
};

struct PIXELICA_OnlineThread {
    HANDLE handle;
    void (*entry)(void *);
    void *arg;
};

PIXELICA_OnlineSync *pixelica_online_sys_sync_create(void)
{
    PIXELICA_OnlineSync *s = (PIXELICA_OnlineSync *)calloc(1, sizeof *s);
    if (s == NULL) return NULL;
    InitializeSRWLock(&s->lock);
    InitializeConditionVariable(&s->cond);
    return s;
}

void pixelica_online_sys_sync_destroy(PIXELICA_OnlineSync *s)
{
    free(s); /* SRW locks and condition variables need no cleanup */
}

void pixelica_online_sys_lock(PIXELICA_OnlineSync *s) { AcquireSRWLockExclusive(&s->lock); }

void pixelica_online_sys_unlock(PIXELICA_OnlineSync *s) { ReleaseSRWLockExclusive(&s->lock); }

void pixelica_online_sys_wait(PIXELICA_OnlineSync *s, uint32_t milliseconds)
{
    (void)SleepConditionVariableSRW(&s->cond, &s->lock, (DWORD)milliseconds, 0);
}

void pixelica_online_sys_wake(PIXELICA_OnlineSync *s) { WakeAllConditionVariable(&s->cond); }

static unsigned __stdcall thread_trampoline(void *arg)
{
    PIXELICA_OnlineThread *t = (PIXELICA_OnlineThread *)arg;
    t->entry(t->arg);
    return 0;
}

PIXELICA_OnlineThread *pixelica_online_sys_thread_start(void (*entry)(void *), void *arg)
{
    PIXELICA_OnlineThread *t = (PIXELICA_OnlineThread *)calloc(1, sizeof *t);
    uintptr_t handle;
    if (t == NULL) return NULL;
    t->entry = entry;
    t->arg = arg;
    handle = _beginthreadex(NULL, 0, thread_trampoline, t, 0, NULL);
    if (handle == 0) {
        free(t);
        return NULL;
    }
    t->handle = (HANDLE)handle;
    return t;
}

void pixelica_online_sys_thread_join(PIXELICA_OnlineThread *t)
{
    if (t == NULL) return;
    (void)WaitForSingleObject(t->handle, INFINITE);
    (void)CloseHandle(t->handle);
    free(t);
}

uint64_t pixelica_online_sys_now_ms(void) { return (uint64_t)GetTickCount64(); }

/* ------------------------------------------------------------------------- */
/* WinHTTP transport                                                          */
/* ------------------------------------------------------------------------- */

typedef struct WinTransport {
    HINTERNET session;
    HINTERNET connection;
    wchar_t path[PIXELICA_ONLINE_URL_MAX + 32u];
    uint32_t timeout_ms;
    bool secure;
} WinTransport;

/* Endpoint strings were validated as ASCII by pixelica_online_internal_parse_url. */
static void widen_ascii(const char *src, wchar_t *dst, size_t capacity)
{
    size_t i = 0;
    for (; src[i] != '\0' && i + 1u < capacity; ++i) dst[i] = (wchar_t)(unsigned char)src[i];
    dst[i] = L'\0';
}

static void win_destroy(void *handle)
{
    WinTransport *t = (WinTransport *)handle;
    if (t == NULL) return;
    if (t->connection != NULL) (void)WinHttpCloseHandle(t->connection);
    if (t->session != NULL) (void)WinHttpCloseHandle(t->session);
    free(t);
}

static void *win_create(const PIXELICA_OnlineEndpoint *endpoint, uint32_t timeout_ms,
    PIXELICA_OnlineStatus *status)
{
    WinTransport *t = (WinTransport *)calloc(1, sizeof *t);
    wchar_t host[256];
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    int timeout = (int)timeout_ms;

    if (t == NULL) {
        *status = PIXELICA_ONLINE_NO_MEMORY;
        return NULL;
    }
    t->timeout_ms = timeout_ms;
    t->secure = endpoint->secure;
    widen_ascii(endpoint->path, t->path, sizeof t->path / sizeof t->path[0]);
    widen_ascii(endpoint->host, host, sizeof host / sizeof host[0]);

#ifdef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
    t->session = WinHttpOpen(L"Pixelica-Online/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
#endif
    if (t->session == NULL) /* pre-8.1 Windows or older SDK */
        t->session = WinHttpOpen(L"Pixelica-Online/1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (t->session == NULL ||
        !WinHttpSetTimeouts(t->session, timeout, timeout, timeout, timeout) ||
        !WinHttpSetOption(t->session, WINHTTP_OPTION_REDIRECT_POLICY, &redirect,
                          (DWORD)sizeof redirect)) {
        win_destroy(t);
        *status = PIXELICA_ONLINE_TRANSPORT;
        return NULL;
    }
    /* IPv6 literals keep their brackets, as WinHTTP expects. */
    t->connection = WinHttpConnect(t->session, host, (INTERNET_PORT)endpoint->port, 0);
    if (t->connection == NULL) {
        win_destroy(t);
        *status = PIXELICA_ONLINE_TRANSPORT;
        return NULL;
    }
    return t;
}

static PIXELICA_OnlineStatus win_post(void *handle, const uint8_t *body, size_t body_size,
    uint8_t *response, size_t capacity, size_t *response_size)
{
    static const wchar_t k_headers[] = L"Content-Type: application/octet-stream\r\n";
    WinTransport *t = (WinTransport *)handle;
    HINTERNET request;
    DWORD autologon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
    DWORD code = 0, code_size = (DWORD)sizeof code;
    ULONGLONG started;
    size_t total = 0;
    PIXELICA_OnlineStatus st = PIXELICA_ONLINE_OK;

    *response_size = 0;
    if (body_size > PIXELICA_ONLINE_PACKET_MAX || capacity > PIXELICA_ONLINE_PACKET_MAX)
        return PIXELICA_ONLINE_INVALID_ARG;
    request = WinHttpOpenRequest(t->connection, L"POST", t->path, NULL, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 (DWORD)(t->secure ? WINHTTP_FLAG_SECURE : 0));
    if (request == NULL) return PIXELICA_ONLINE_TRANSPORT;
    if (!WinHttpSetOption(request, WINHTTP_OPTION_AUTOLOGON_POLICY, &autologon,
                          (DWORD)sizeof autologon)) {
        (void)WinHttpCloseHandle(request);
        return PIXELICA_ONLINE_TRANSPORT;
    }

    started = GetTickCount64();
    if (!WinHttpSendRequest(request, k_headers,
                            (DWORD)(sizeof k_headers / sizeof k_headers[0] - 1u),
                            (LPVOID)body, (DWORD)body_size, (DWORD)body_size, 0) ||
        !WinHttpReceiveResponse(request, NULL) ||
        !WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &code, &code_size,
                             WINHTTP_NO_HEADER_INDEX)) {
        st = PIXELICA_ONLINE_TRANSPORT;
    } else {
        st = pixelica_online_internal_http_status((long)code);
    }

    while (st == PIXELICA_ONLINE_OK) {
        DWORD got = 0;
        BOOL ok;
        uint8_t extra;
        if (total < capacity) {
            ok = WinHttpReadData(request, response + total, (DWORD)(capacity - total), &got);
        } else {
            ok = WinHttpReadData(request, &extra, 1, &got);
            if (ok && got != 0) st = PIXELICA_ONLINE_PROTOCOL; /* larger than any valid reply */
        }
        if (!ok) st = PIXELICA_ONLINE_TRANSPORT;
        if (st != PIXELICA_ONLINE_OK || got == 0) break;
        total += got;
        if (GetTickCount64() - started > (ULONGLONG)t->timeout_ms) st = PIXELICA_ONLINE_TRANSPORT;
    }

    (void)WinHttpCloseHandle(request);
    if (st == PIXELICA_ONLINE_OK) *response_size = total;
    return st;
}

static const PIXELICA_OnlineTransportOps k_winhttp_ops = { win_create, win_post, win_destroy };

const PIXELICA_OnlineTransportOps *pixelica_online_sys_transport(void) { return &k_winhttp_ops; }
