/* Linux platform layer for the online module: pthread mutex + monotonic
 * condition variable, a pthread worker, CLOCK_MONOTONIC and a libcurl
 * transport (one easy handle per client, so connections are reused).
 *
 * curl_global_init runs once through pthread_once and is never cleaned up, so
 * clients created and destroyed at any time never pull libcurl out from under
 * other users in the process. TLS peer and host verification stay on (libcurl
 * defaults, set explicitly), redirects are off, only the endpoint's own scheme
 * is allowed, signals are not used (CURLOPT_NOSIGNAL) and CURLOPT_TIMEOUT_MS
 * caps the whole request. */
#if !defined(_POSIX_C_SOURCE) || _POSIX_C_SOURCE < 200809L
#undef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <curl/curl.h>

#include "online_internal.h"

struct PIXELICA_OnlineSync {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
};

struct PIXELICA_OnlineThread {
    pthread_t thread;
    void (*entry)(void *);
    void *arg;
};

PIXELICA_OnlineSync *pixelica_online_sys_sync_create(void)
{
    PIXELICA_OnlineSync *s = (PIXELICA_OnlineSync *)calloc(1, sizeof *s);
    pthread_condattr_t attr;
    if (s == NULL) return NULL;
    if (pthread_mutex_init(&s->mutex, NULL) != 0) {
        free(s);
        return NULL;
    }
    if (pthread_condattr_init(&attr) != 0) {
        (void)pthread_mutex_destroy(&s->mutex);
        free(s);
        return NULL;
    }
    if (pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) != 0 ||
        pthread_cond_init(&s->cond, &attr) != 0) {
        (void)pthread_condattr_destroy(&attr);
        (void)pthread_mutex_destroy(&s->mutex);
        free(s);
        return NULL;
    }
    (void)pthread_condattr_destroy(&attr);
    return s;
}

void pixelica_online_sys_sync_destroy(PIXELICA_OnlineSync *s)
{
    if (s == NULL) return;
    (void)pthread_cond_destroy(&s->cond);
    (void)pthread_mutex_destroy(&s->mutex);
    free(s);
}

void pixelica_online_sys_lock(PIXELICA_OnlineSync *s) { (void)pthread_mutex_lock(&s->mutex); }

void pixelica_online_sys_unlock(PIXELICA_OnlineSync *s) { (void)pthread_mutex_unlock(&s->mutex); }

void pixelica_online_sys_wait(PIXELICA_OnlineSync *s, uint32_t milliseconds)
{
    struct timespec deadline;
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) return;
    deadline.tv_sec += (time_t)(milliseconds / 1000u);
    deadline.tv_nsec += (long)(milliseconds % 1000u) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }
    (void)pthread_cond_timedwait(&s->cond, &s->mutex, &deadline);
}

void pixelica_online_sys_wake(PIXELICA_OnlineSync *s) { (void)pthread_cond_broadcast(&s->cond); }

static void *thread_trampoline(void *arg)
{
    PIXELICA_OnlineThread *t = (PIXELICA_OnlineThread *)arg;
    t->entry(t->arg);
    return NULL;
}

PIXELICA_OnlineThread *pixelica_online_sys_thread_start(void (*entry)(void *), void *arg)
{
    PIXELICA_OnlineThread *t = (PIXELICA_OnlineThread *)calloc(1, sizeof *t);
    if (t == NULL) return NULL;
    t->entry = entry;
    t->arg = arg;
    if (pthread_create(&t->thread, NULL, thread_trampoline, t) != 0) {
        free(t);
        return NULL;
    }
    return t;
}

void pixelica_online_sys_thread_join(PIXELICA_OnlineThread *t)
{
    if (t == NULL) return;
    (void)pthread_join(t->thread, NULL);
    free(t);
}

uint64_t pixelica_online_sys_now_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

/* ------------------------------------------------------------------------- */
/* libcurl transport                                                          */
/* ------------------------------------------------------------------------- */

static pthread_once_t g_curl_once = PTHREAD_ONCE_INIT;
static CURLcode g_curl_init = CURLE_FAILED_INIT;

static void curl_init_once(void) { g_curl_init = curl_global_init(CURL_GLOBAL_DEFAULT); }

typedef struct CurlTransport {
    CURL *easy;
    struct curl_slist *headers;
} CurlTransport;

typedef struct CurlSink {
    uint8_t *data;
    size_t capacity;
    size_t size;
    bool overflow;
} CurlSink;

static size_t curl_sink_write(char *ptr, size_t size, size_t count, void *userdata)
{
    CurlSink *sink = (CurlSink *)userdata;
    size_t total;
    if (count != 0 && size > (size_t)-1 / count) {
        sink->overflow = true;
        return 0;
    }
    total = size * count;
    if (total > sink->capacity - sink->size) {
        sink->overflow = true; /* larger than any valid reply: abort */
        return 0;
    }
    if (total != 0) memcpy(sink->data + sink->size, ptr, total);
    sink->size += total;
    return total;
}

static void curl_destroy(void *handle)
{
    CurlTransport *t = (CurlTransport *)handle;
    if (t == NULL) return;
    if (t->easy != NULL) curl_easy_cleanup(t->easy);
    curl_slist_free_all(t->headers);
    free(t);
}

static bool append_header(CurlTransport *t, const char *line)
{
    struct curl_slist *next = curl_slist_append(t->headers, line);
    if (next == NULL) return false;
    t->headers = next;
    return true;
}

static void *curl_create(const PIXELICA_OnlineEndpoint *endpoint, uint32_t timeout_ms,
    PIXELICA_OnlineStatus *status)
{
    CurlTransport *t;
    CURLcode rc = CURLE_OK;
    long timeout = (long)timeout_ms;

    (void)pthread_once(&g_curl_once, curl_init_once);
    if (g_curl_init != CURLE_OK) {
        *status = PIXELICA_ONLINE_TRANSPORT;
        return NULL;
    }
    t = (CurlTransport *)calloc(1, sizeof *t);
    if (t == NULL) {
        *status = PIXELICA_ONLINE_NO_MEMORY;
        return NULL;
    }
    t->easy = curl_easy_init();
    if (t->easy == NULL || !append_header(t, "Content-Type: application/octet-stream") ||
        !append_header(t, "Expect:")) {
        curl_destroy(t);
        *status = PIXELICA_ONLINE_NO_MEMORY;
        return NULL;
    }

    if (rc == CURLE_OK) rc = curl_easy_setopt(t->easy, CURLOPT_URL, endpoint->url);
#if LIBCURL_VERSION_NUM >= 0x075500 /* 7.85.0 */
    if (rc == CURLE_OK)
        rc = curl_easy_setopt(t->easy, CURLOPT_PROTOCOLS_STR, endpoint->secure ? "https" : "http");
#else
    if (rc == CURLE_OK)
        rc = curl_easy_setopt(t->easy, CURLOPT_PROTOCOLS,
                              endpoint->secure ? (long)CURLPROTO_HTTPS : (long)CURLPROTO_HTTP);
#endif
    if (rc == CURLE_OK) rc = curl_easy_setopt(t->easy, CURLOPT_NOSIGNAL, 1L);
    if (rc == CURLE_OK) rc = curl_easy_setopt(t->easy, CURLOPT_FOLLOWLOCATION, 0L);
    if (rc == CURLE_OK) rc = curl_easy_setopt(t->easy, CURLOPT_SSL_VERIFYPEER, 1L);
    if (rc == CURLE_OK) rc = curl_easy_setopt(t->easy, CURLOPT_SSL_VERIFYHOST, 2L);
    if (rc == CURLE_OK) rc = curl_easy_setopt(t->easy, CURLOPT_TIMEOUT_MS, timeout);
    if (rc == CURLE_OK) rc = curl_easy_setopt(t->easy, CURLOPT_CONNECTTIMEOUT_MS, timeout);
    if (rc == CURLE_OK) rc = curl_easy_setopt(t->easy, CURLOPT_POST, 1L);
    if (rc == CURLE_OK) rc = curl_easy_setopt(t->easy, CURLOPT_HTTPHEADER, t->headers);
    if (rc == CURLE_OK) rc = curl_easy_setopt(t->easy, CURLOPT_USERAGENT, "Pixelica-Online/1");
    if (rc == CURLE_OK)
        rc = curl_easy_setopt(t->easy, CURLOPT_MAXFILESIZE, (long)PIXELICA_ONLINE_PACKET_MAX);
    if (rc == CURLE_OK) rc = curl_easy_setopt(t->easy, CURLOPT_WRITEFUNCTION, curl_sink_write);
    if (rc != CURLE_OK) {
        curl_destroy(t);
        *status = PIXELICA_ONLINE_UNSUPPORTED;
        return NULL;
    }
    return t;
}

static PIXELICA_OnlineStatus curl_post(void *handle, const uint8_t *body, size_t body_size,
    uint8_t *response, size_t capacity, size_t *response_size)
{
    CurlTransport *t = (CurlTransport *)handle;
    CurlSink sink;
    CURLcode rc = CURLE_OK;
    long code = 0;

    *response_size = 0;
    if (body_size > PIXELICA_ONLINE_PACKET_MAX) return PIXELICA_ONLINE_INVALID_ARG;
    sink.data = response;
    sink.capacity = capacity;
    sink.size = 0;
    sink.overflow = false;

    if (rc == CURLE_OK) rc = curl_easy_setopt(t->easy, CURLOPT_POSTFIELDS, (const void *)body);
    if (rc == CURLE_OK) rc = curl_easy_setopt(t->easy, CURLOPT_POSTFIELDSIZE, (long)body_size);
    if (rc == CURLE_OK) rc = curl_easy_setopt(t->easy, CURLOPT_WRITEDATA, (void *)&sink);
    if (rc == CURLE_OK) rc = curl_easy_perform(t->easy);
    /* Do not leave pointers to caller/stack memory inside the handle. */
    (void)curl_easy_setopt(t->easy, CURLOPT_POSTFIELDS, (const void *)NULL);
    (void)curl_easy_setopt(t->easy, CURLOPT_WRITEDATA, (void *)NULL);

    if (sink.overflow || rc == CURLE_FILESIZE_EXCEEDED) return PIXELICA_ONLINE_PROTOCOL;
    if (rc != CURLE_OK) return PIXELICA_ONLINE_TRANSPORT;
    if (curl_easy_getinfo(t->easy, CURLINFO_RESPONSE_CODE, &code) != CURLE_OK)
        return PIXELICA_ONLINE_TRANSPORT;
    if (pixelica_online_internal_http_status(code) != PIXELICA_ONLINE_OK)
        return pixelica_online_internal_http_status(code);
    *response_size = sink.size;
    return PIXELICA_ONLINE_OK;
}

static const PIXELICA_OnlineTransportOps k_curl_ops = { curl_create, curl_post, curl_destroy };

const PIXELICA_OnlineTransportOps *pixelica_online_sys_transport(void) { return &k_curl_ops; }
