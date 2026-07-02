#include "postgres.h"
#include "utils/memutils.h"
#include "utils/jsonb.h"
#include "lib/stringinfo.h"

#include <curl/curl.h>
#include <time.h>
#include "nodes/pg_list.h"

#include "https_core.h"
#include "utils/builtins.h"
#include "miscadmin.h" // for CHECK_FOR_INTERRUPTS()

#include "utils/lsyscache.h"
#include "catalog/pg_type.h"
#include "utils/fmgrprotos.h"

/* 
 * initially used jsonb_iterator library but that fails with psql 17
 * psql compresses the data hence _PP but there's no such for jsonb , so need to use _P for jsonb (also jsonb isn't flat string )
 */

extern Datum jsonb_in(PG_FUNCTION_ARGS);

/*
 *  GUC backed globals, defined in pg_https
 */
extern int pg_https_max_response_size;
extern char *pg_https_default_headers;
extern char *pg_https_ca_file;

/*
 * simple buf wrapper for libcurl callbacks
 *  uses pg's stringinfo , auto growing buf
 */
struct curl_buffer {
    StringInfoData data;
};

typedef struct https_curl_ctx
{
    https_cancel_mode cancel_mode;
} https_curl_ctx;

/* Function prototypes */
static struct curl_slist *merge_headers(struct curl_slist *default_list, struct curl_slist *user_list);
static int progress_callback(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow);
static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp);
static size_t header_callback(char *buffer, size_t size, size_t nitems, void *userdata);
static struct curl_slist *build_headers(Jsonb *headers_jsonb, bool *has_auth_header);
static bool is_valid_header_name(const char *k);
static bool is_valid_header_value(const char *v);
static HTAB *parse_headers_to_map(char *raw_headers);
static void headers_map_to_jsonb(HTAB *htab, JsonbParseState **state);
static bool is_idempotent(const char *method);
static bool is_retryable(CURLcode res);
static void sleepy(long delay, long jitter);

/*
 * merge default and user list safely
 */
static struct curl_slist *
merge_headers(struct curl_slist *default_list, struct curl_slist *user_list)
{
    struct curl_slist *tmp;

    if (!default_list) return user_list;
    if (!user_list) return default_list;

    tmp = default_list;
    while (tmp->next)
        tmp = tmp->next;
    tmp->next = user_list;
    return default_list;
}

/*
 *  called periodically during transfer
 *  !!! IMPORTANT !!! : allows query cancellation and stmnt timeout to imtr long running HTTP calls
 */
static int
progress_callback(void *clientp,
                  curl_off_t dltotal, curl_off_t dlnow,
                  curl_off_t ultotal, curl_off_t ulnow)
{
    https_curl_ctx *ctx;

    if (QueryCancelPending || ProcDiePending)
    {
        ctx = (https_curl_ctx *) clientp;
        if (ctx)
        {
            if (ctx->cancel_mode == HTTPS_CANCEL_ABORT)
                return 1;  /* abort curl, hope for gracefully */
            if (ctx->cancel_mode == HTTPS_CANCEL_WAIT)
                return 0;  /* ignore cancel, continue request */
        }
    }
    return 0;
}

/* 
 * write callback, response body
 *  - appends incoming chunks into buf + enforces max resp size 
 *  returning 0 signals libcurl to abort the transfer
 */
static size_t
write_callback(void *contents, size_t size, size_t nmemb, void *userp)
{
    struct curl_buffer *mem;
    size_t realsize;

    if (!userp || !contents)
        return size * nmemb;
    
    mem = (struct curl_buffer *) userp;
    if (!mem)
        return 0;
    
    /* overflow protection */
    if (nmemb != 0 && size > SIZE_MAX / nmemb)
        return 0;

    realsize = size * nmemb;

    /* enforce max response size */
    if (mem->data.len + realsize > pg_https_max_response_size)
        return 0;

    appendBinaryStringInfo(&mem->data, contents, realsize);

    return realsize;
}

/* 
 * header callback, collect raw headers for parsing 
 */
static size_t
header_callback(char *buffer, size_t size, size_t nitems, void *userdata)
{
    size_t realsize;
    struct curl_buffer *mem;

    if (!buffer || !userdata)
        return 0;

    realsize = size * nitems;
    mem = (struct curl_buffer *) userdata;

    if (mem->data.len + realsize > pg_https_max_response_size)
        return 0;

    appendBinaryStringInfo(&mem->data, buffer, realsize);
    return realsize;
}

/*
 *  build headers from JSONB
 *  supports non-string jsonb values by serializing them and tracks if auth header present
 */
static struct curl_slist *
build_headers(Jsonb *headers_jsonb, bool *has_auth_header)
{
    struct curl_slist *chunk = NULL;
    JsonbIterator *it;
    JsonbValue v;
    JsonbIteratorToken r;
    char *key = NULL;
    char *val = NULL;
    Jsonb *tmp = NULL;
    StringInfoData buf;
    StringInfoData header;

    if (has_auth_header)
        *has_auth_header = false;

    if (!headers_jsonb)
        return NULL;

    if (!JB_ROOT_IS_OBJECT(headers_jsonb))
        ereport(ERROR, (errmsg("headers must be a JSON object")));

    it = JsonbIteratorInit(&headers_jsonb->root);

    while ((r = JsonbIteratorNext(&it, &v, false)) != WJB_DONE)
    {
        if (r == WJB_KEY)
        {
            key = pnstrdup(v.val.string.val, v.val.string.len);

            r = JsonbIteratorNext(&it, &v, false);  /* get VALUE */

            if (r != WJB_VALUE)
            {
                pfree(key);
                continue;
            }

            /*
             *  fast path : string vals
             *  fallback : serialize JSON value into string
             */
            if (v.type == jbvString)
            {
                val = pnstrdup(v.val.string.val, v.val.string.len);
            }
            else
            {
                tmp = JsonbValueToJsonb(&v);

                initStringInfo(&buf);
                JsonbToCString(&buf, &tmp->root, VARSIZE_ANY(tmp));
                val = pnstrdup(buf.data, buf.len);

                pfree(buf.data);
            }

            initStringInfo(&header);

            appendStringInfo(&header, "%s: %s", key, val);
            if (pg_strcasecmp(key, "authorization") == 0)
                *has_auth_header = true;
            chunk = curl_slist_append(chunk, header.data);

            pfree(key);
            pfree(val);
            pfree(header.data);
        }
    }

    return chunk;
}

/* 
 * header name validations
 * HTTP spec RFC says invalid names , only A-Z a-z 0-9 !#$%&'*+-.^_`|~ allowed
 */
static bool
is_valid_header_name(const char *k)
{
    const char *p;
    for (p = k; *p; p++)
    {
        if (!(
                (*p >= 'a' && *p <= 'z') ||
                (*p >= 'A' && *p <= 'Z') ||
                (*p >= '0' && *p <= '9') ||
                *p == '!' || *p == '#' || *p == '$' ||
                *p == '%' || *p == '&' || *p == '\'' ||
                *p == '*' || *p == '+' || *p == '-' ||
                *p == '.' || *p == '^' || *p == '_' ||
                *p == '`' || *p == '|' || *p == '~'
            ))
            return false;
    }    
    return true;
}

/* header val validations */
static bool
is_valid_header_value(const char *v)
{
    const char *p;
    for (p = v; *p; p++)
    {
        if (*p == '\r' || *p == '\n')
            return false;
    }
    return true;
}

/* used for header parsing , key val pairing */
static HTAB *
parse_headers_to_map(char *raw_headers)
{
    HASHCTL ctl;
    HTAB *htab;
    char *line;
    char *saveptr = NULL;
    char *colon;
    char *k;
    char *v;
    char *end;
    char *p;
    char lower_key[256];
    bool found;
    header_entry *entry;

    memset(&ctl, 0, sizeof(ctl));
    ctl.keysize = 256;
    ctl.entrysize = sizeof(header_entry);

#ifdef HASH_STRINGS
    htab = hash_create("headers map", 32, &ctl, HASH_ELEM | HASH_STRINGS);
#else
    htab = hash_create("headers map", 32, &ctl, HASH_ELEM);
#endif

    for (line = strtok_r(raw_headers, "\r\n", &saveptr);
         line != NULL;
         line = strtok_r(NULL, "\r\n", &saveptr))
    {
        /* If the line starts with HTTP/ (case-insensitive), it's a status line. 
         * This indicates a new redirect/response block. Reset the map.
         */
        if (pg_strncasecmp(line, "HTTP/", 5) == 0)
        {
            /* Destroy the existing hash table and create a new one to discard
             * headers from previous redirects / 100-continue blocks.
             */
            hash_destroy(htab);
#ifdef HASH_STRINGS
            htab = hash_create("headers map", 32, &ctl, HASH_ELEM | HASH_STRINGS);
#else
            htab = hash_create("headers map", 32, &ctl, HASH_ELEM);
#endif
            continue;
        }

        colon = strchr(line, ':');
        if (!colon)
            continue;

        *colon = '\0';
        k = line;
        v = colon + 1;

        /* Trim leading whitespace from value */
        while (*v == ' ' || *v == '\t')
            v++;

        /* Trim trailing whitespace from value */
        end = v + strlen(v) - 1;
        while (end >= v && (*end == ' ' || *end == '\t'))
        {
            *end = '\0';
            end--;
        }

        /* Validation: skip invalid headers rather than throwing error */
        if (strlen(k) == 0 || strlen(k) > 255)
            continue;

        if (!is_valid_header_name(k))
            continue;

        if (!is_valid_header_value(v))
            continue;

        /* Case-insensitive key normalisation */
        memset(lower_key, 0, sizeof(lower_key));
        strlcpy(lower_key, k, sizeof(lower_key));
        for (p = lower_key; *p; p++)
            *p = pg_tolower((unsigned char)*p);

        entry = hash_search(htab, lower_key, HASH_ENTER, &found);
        if (!found)
        {
            entry->values = NIL;
        }

        entry->values = lappend(entry->values, pstrdup(v));
    }

    return htab;
}

static void
headers_map_to_jsonb(HTAB *htab, JsonbParseState **state)
{
    HASH_SEQ_STATUS status;
    header_entry *entry;
    JsonbValue key;
    JsonbValue val;
    char *val_str;
    ListCell *lc;

    hash_seq_init(&status, htab);

    while ((entry = hash_seq_search(&status)) != NULL)
    {
        key.type = jbvString;
        key.val.string.val = entry->key;
        key.val.string.len = strlen(entry->key);

        pushJsonbValue(state, WJB_KEY, &key);

        if (list_length(entry->values) == 1)
        {
            val_str = linitial(entry->values);
            val.type = jbvString;
            val.val.string.val = val_str;
            val.val.string.len = strlen(val_str);

            pushJsonbValue(state, WJB_VALUE, &val);
        }
        else
        {
            pushJsonbValue(state, WJB_BEGIN_ARRAY, NULL);

            foreach(lc, entry->values)
            {
                val_str = lfirst(lc);
                val.type = jbvString;
                val.val.string.val = val_str;
                val.val.string.len = strlen(val_str);

                pushJsonbValue(state, WJB_ELEM, &val);
            }

            pushJsonbValue(state, WJB_END_ARRAY, NULL);
        }
    }
}

/*
 * only retry idempotent methods
 */
static bool
is_idempotent(const char *method)
{
    return (
        pg_strcasecmp(method, "GET") == 0 ||
        pg_strcasecmp(method, "HEAD") == 0 ||
        pg_strcasecmp(method, "PUT") == 0 ||
        pg_strcasecmp(method, "DELETE") == 0 ||
        pg_strcasecmp(method, "OPTIONS") == 0
    );
}

/*
 * retry only for transient net err
 */
static bool
is_retryable(CURLcode res)
{
    switch (res)
    {
        case CURLE_OPERATION_TIMEDOUT:
        case CURLE_COULDNT_CONNECT:
        case CURLE_RECV_ERROR:
        case CURLE_SEND_ERROR:
        case CURLE_GOT_NOTHING:
        case CURLE_PARTIAL_FILE:
        case CURLE_HTTP2:
            return true;
        default:
            return false;
    }
}

static void
sleepy(long delay, long jitter)
{
    long remaining_us = (long)(delay + jitter) * 1000L;
    const long chunk_us = 100000L;  /* 100ms chunks */
    while (remaining_us > 0)
    {
        CHECK_FOR_INTERRUPTS();  /* safe here — not inside libcurl */
        pg_usleep(Min(chunk_us, remaining_us));
        remaining_us -= chunk_us;
    }
}

/*
 *  main http req execution 
 */
https_result*
https_execute(
    const char *url, const char *method, Jsonb *headers_jsonb, const char *req_body, int req_body_len,
    const int timeout_override,
    const char *username, const char *password,
    int retries, int retry_delay_ms, double retry_backoff,
    int cancel_mode 
)
{
    MemoryContext old_ctx;
    MemoryContext req_ctx;
    bool has_auth_header;
    bool user_has_auth;
    bool default_has_auth;
    CURLcode res;
    long http_code;
    struct curl_slist *curl_headers;
    https_result *result;
    struct curl_buffer response_body;
    struct curl_buffer response_headers;
    struct timespec start, end;
    int duration_ms;
    int effective_timeout;
    long curl_tls_version;
    JsonbParseState *state;
    Jsonb *default_headers_jsonb;
    struct curl_slist *default_list;
    struct curl_slist *user_list;
    int attempt;
    int delay;
    JsonbValue key, val;
    https_curl_ctx ctx;
    int tcp_keepalive;
    CURL *curl;
    int max_delay;
    char errbuf[CURL_ERROR_SIZE];
    int jitter;
    char *headers_copy;
    JsonbValue *jb_res;
    HTAB *map;

    /* Initialize GUC/local variables */
    has_auth_header = false;
    user_has_auth = false;
    default_has_auth = false;
    http_code = 0;
    curl_headers = NULL;
    duration_ms = 0;
    effective_timeout = https_timeout;
    curl_tls_version = CURL_SSLVERSION_DEFAULT;
    state = NULL;
    default_headers_jsonb = NULL;
    default_list = NULL;
    user_list = NULL;
    attempt = 0;
    delay = retry_delay_ms;
    tcp_keepalive = pg_https_tcp_keepalive;
    max_delay = 10000;
    headers_copy = NULL;
    ctx.cancel_mode = cancel_mode;

    /* Allocate short-lived memory context for request-scoped variables */
    req_ctx = AllocSetContextCreate(CurrentMemoryContext, "pg_https req ctx", ALLOCSET_DEFAULT_SIZES);
    old_ctx = MemoryContextSwitchTo(req_ctx);

    curl = curl_easy_init();
    if (!curl)
    {
        MemoryContextSwitchTo(old_ctx);
        MemoryContextDelete(req_ctx);
        ereport(ERROR, (errmsg("Failed to init curl")));
    }

    PG_TRY();
    {
        /* parse default headers per req */
        if (pg_https_default_headers && strlen(pg_https_default_headers) > 0)
        {
            default_headers_jsonb = DatumGetJsonbP(
                DirectFunctionCall1(jsonb_in,
                    CStringGetDatum(pg_https_default_headers)));
        }

        /* append user headers to default headers */
        default_list = build_headers(default_headers_jsonb, &default_has_auth);
        user_list    = build_headers(headers_jsonb, &user_has_auth);

        has_auth_header = user_has_auth || default_has_auth;
        curl_headers = merge_headers(default_list, user_list);

        initStringInfo(&response_body.data);
        initStringInfo(&response_headers.data);

        clock_gettime(CLOCK_MONOTONIC, &start);

        /* ---- curl config ---- */
        if (!url || strlen(url) == 0)
            ereport(ERROR, (errmsg("URL cannot be empty")));
        curl_easy_setopt(curl, CURLOPT_URL, url);

        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *) &response_body);

        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, (void *) &response_headers);

        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, https_connect_timeout);

        effective_timeout = (timeout_override > 0) ? timeout_override : https_timeout;
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, effective_timeout);

        /* TLS/ssl version handling */
        if (https_tls_version == 12)
            curl_tls_version = CURL_SSLVERSION_TLSv1_2;
        else if (https_tls_version == 13)
            curl_tls_version = CURL_SSLVERSION_TLSv1_3;
        else if (https_tls_version != 0)
            ereport(ERROR, (errmsg("invalid tls_version: %d", https_tls_version)));

        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, https_verify_peer ? 1L : 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, https_verify_peer ? 2L : 0L);
        curl_easy_setopt(curl, CURLOPT_SSLVERSION, curl_tls_version);

        if (pg_https_ca_file && strlen(pg_https_ca_file) > 0)
        {
            curl_easy_setopt(curl, CURLOPT_CAINFO, pg_https_ca_file);
        }

        /* HTTP/2 */
        curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, (pg_https_http_version == 2) ? CURL_HTTP_VERSION_2TLS : CURL_HTTP_VERSION_1_1);

        /* ALPN only when HTTP/2 is explicitly requested */
        if (pg_https_http_version == 2)
            curl_easy_setopt(curl, CURLOPT_SSL_ENABLE_ALPN, 1L);

        /* Compression, sys default */
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

        /* Interrupt + hang fix */
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

        /* tcp keep-alive */
        if (tcp_keepalive > 0)
        {
            curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
            curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, tcp_keepalive);
            curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, 60L);
#ifdef CURLOPT_TCP_KEEPCNT
            curl_easy_setopt(curl, CURLOPT_TCP_KEEPCNT, 3L);
#endif
        }

        /* prevent slow/stall/hanging conns */
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, https_connect_timeout);

        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "pg_https/1.1");
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

        /* Connection reuse GUC configuration */
        if (!https_connection_reuse)
        {
            curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 1L);
            curl_easy_setopt(curl, CURLOPT_FRESH_CONNECT, 1L);
        }

        if (curl_headers)
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, curl_headers);

        /* apply basic auth only if no auth header */
        if (!has_auth_header && username && *username)
        {
            curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
            curl_easy_setopt(curl, CURLOPT_USERNAME, username);
            if (password)
                curl_easy_setopt(curl, CURLOPT_PASSWORD, password);
        }

        /* method handling */
        if (!method)
            ereport(ERROR, (errmsg("HTTP method cannot be NULL")));

        if (pg_strcasecmp(method, "GET") == 0)
        {
            curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
        }
        else
        {
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
            if (req_body && req_body_len > 0)
            {
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req_body);
                curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t) req_body_len);
            }
        }

        errbuf[0] = '\0';
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);

        for (attempt = 0; attempt <= retries; attempt++)
        {
            if (attempt > 0)
            {
                ereport(LOG, (errmsg("pg_https retry %d for %s %s", attempt, method, url)));
            }
            CHECK_FOR_INTERRUPTS();
            resetStringInfo(&response_body.data);
            resetStringInfo(&response_headers.data);
            http_code = 0;

            res = curl_easy_perform(curl);

            ereport(LOG, (errmsg("pg_https: curl : attempt %d ,result=%d (%s)", attempt, res, curl_easy_strerror(res))));
            
            if (res == CURLE_ABORTED_BY_CALLBACK)
            {
                MemoryContextSwitchTo(old_ctx);
                result = palloc0(sizeof(https_result));
                result->curl_error_code = CURLE_ABORTED_BY_CALLBACK;
                result->error_message = pstrdup("query cancelled");
                result->headers = DatumGetJsonbP(
                    DirectFunctionCall1(jsonb_in, CStringGetDatum("{}")));

                goto cleanup;
            }
            if (res == CURLE_OK)
            {
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            
                if (http_code < 500 && http_code != 408 && http_code != 429)
                    break;

                if (!is_idempotent(method))
                    break;
            }
            else
            {
                if (!is_retryable(res) || !is_idempotent(method))
                    break;
            }

            if (attempt == retries)
                break;

#if PG_VERSION_NUM >= 150000
            jitter = pg_prng_uint32(&pg_global_prng_state) % 100;
#else
            jitter = random() % 100;
#endif
            sleepy(delay, jitter);
            delay = (int)(delay * retry_backoff);
            if (delay > max_delay)
                delay = max_delay;
        }
    
        if (res == CURLE_WRITE_ERROR)
        {
            MemoryContextSwitchTo(old_ctx);
            result = palloc0(sizeof(https_result));
            result->status = 0;
            result->curl_error_code = res;
            result->error_message = pstrdup("response exceeded max_response_size limit");
            result->headers = DatumGetJsonbP(
                DirectFunctionCall1(jsonb_in, CStringGetDatum("{}")));
            goto cleanup;
        }

        if (res != CURLE_OK)
        {
            MemoryContextSwitchTo(old_ctx);
            result = palloc0(sizeof(https_result));
            result->status = 0;
            result->body = psprintf("{\"error\":true,\"message\":\"%s\",\"detail\":\"%s\"}", curl_easy_strerror(res), errbuf[0] ? errbuf : "no detail");
            result->headers = DatumGetJsonbP(
                DirectFunctionCall1(jsonb_in, CStringGetDatum("{}")));
            result->duration_ms = 0;
            result->response_bytes = 0;
            result->curl_error_code = res;
            goto cleanup;
        }

        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        clock_gettime(CLOCK_MONOTONIC, &end);

        duration_ms = (end.tv_sec - start.tv_sec) * 1000 +
                      (end.tv_nsec - start.tv_nsec) / 1000000;

        /* ---- build result ---- */
        MemoryContextSwitchTo(old_ctx);
        result = palloc(sizeof(https_result));

        result->status = (int) http_code;

        if (response_body.data.data && response_body.data.len > 0)
            result->body = pnstrdup(response_body.data.data, response_body.data.len);
        else
            result->body = pstrdup("");

        result->error_message = NULL;

        pushJsonbValue(&state, WJB_BEGIN_OBJECT, NULL);
        
        /* 1. Add "raw" header field */
        key.type = jbvString;
        key.val.string.val = "raw";
        key.val.string.len = 3;

        val.type = jbvString;
        if (response_headers.data.len > 0 && response_headers.data.data)
        {
            val.val.string.val = pnstrdup(response_headers.data.data, response_headers.data.len);
            val.val.string.len = response_headers.data.len;
        }
        else
        {
            val.val.string.val = "";
            val.val.string.len = 0;
        }

        pushJsonbValue(&state, WJB_KEY, &key);
        pushJsonbValue(&state, WJB_VALUE, &val);

        /* 2. Parse and add individual headers if any */
        if (response_headers.data.len > 0 && response_headers.data.data)
        {
            headers_copy = pnstrdup(response_headers.data.data, response_headers.data.len);
            map = parse_headers_to_map(headers_copy);
            headers_map_to_jsonb(map, &state);
            hash_destroy(map);
        }

        jb_res = pushJsonbValue(&state, WJB_END_OBJECT, NULL);
        if (!jb_res)
            ereport(ERROR, (errmsg("failed to build jsonb headers")));
        
        result->headers = JsonbValueToJsonb(jb_res);

        result->duration_ms = duration_ms;
        result->response_bytes = response_body.data.len;
        result->curl_error_code = res;

        goto cleanup;

        cleanup:
            if (curl_headers)
                curl_slist_free_all(curl_headers);
            else
            {
                if (default_list)
                    curl_slist_free_all(default_list);
                if (user_list)
                    curl_slist_free_all(user_list);
            }
        
            if (curl)
                curl_easy_cleanup(curl);

            MemoryContextDelete(req_ctx);
    }
    PG_CATCH();
    {
        if (curl)
            curl_easy_cleanup(curl);
        if (curl_headers)
            curl_slist_free_all(curl_headers);
        else
        {
            if (default_list)
                curl_slist_free_all(default_list);
            if (user_list)
                curl_slist_free_all(user_list);
        }
        MemoryContextSwitchTo(old_ctx);
        MemoryContextDelete(req_ctx);
        PG_RE_THROW();
    }
    PG_END_TRY();

    CHECK_FOR_INTERRUPTS(); 
    return result;
}

void
pg_https_cleanup(int code, Datum arg)
{
    curl_global_cleanup();
}
