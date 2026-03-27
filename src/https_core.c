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
// #include "utils/interrupt.h"

/* 
 * initially used jsonb_iterator library but that fails with psql 17
 * psql compresses the data hence _PP but there's no such for jsonb , so need to use _P for jsonb (also jsonb isn't flat string )
 * 
 */



extern Datum jsonb_in(PG_FUNCTION_ARGS);

// #define MAX_RESPONSE_SIZE (10 * 1024 * 1024)  // 10MB// GUC taken

/*
 *  GUC backed globals, defined in pg_https
*/

extern int pg_https_max_response_size;
extern char *pg_https_default_headers;

/*
 *  intended for caching parsed default header but currently unused ( parsing happens per req )
*/

static Jsonb *cached_default_headers = NULL;

/*
 * simple buf wrapper for libcurl callbacks
 *  uses pg's stringinfo , auto growing buf
*/

struct curl_buffer {
    StringInfoData data;
};

/*
 *  called periodically during transfer
 *  !!! IMPORTANT !!! : allows query cancellation and stmnt timeout to imtr long running HTTP calls
*/

static int
progress_callback(void *clientp,
                  curl_off_t dltotal, curl_off_t dlnow,
                  curl_off_t ultotal, curl_off_t ulnow)
{
    CHECK_FOR_INTERRUPTS();  // allow cancel / statement_timeout
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
    size_t realsize = size * nmemb;
    struct curl_buffer *mem = (struct curl_buffer *) userp;
    
    //what if the size too big
    // if (mem->data.len + realsize > MAX_RESPONSE_SIZE)
    if (mem->data.len + realsize > pg_https_max_response_size)
        return 0;  // abort transfer
    // if (res == CURLE_WRITE_ERROR)
    //     ereport(ERROR, (errmsg("response too large")));

    appendBinaryStringInfo(&mem->data, contents, realsize);
    return realsize;
}

/* 
 * header callback, collect raw headers for parsing 
 */
static size_t
header_callback(char *buffer, size_t size, size_t nitems, void *userdata)
{
    size_t realsize = size * nitems;
    struct curl_buffer *mem = (struct curl_buffer *) userdata;

    // appendBinaryStringInfo(&mem->data, buffer, realsize);
    if (mem->data.len + realsize > pg_https_max_response_size)
        return 0;

    appendBinaryStringInfo(&mem->data, buffer, realsize);
    return realsize;
}

/*
 *  build headers from JSONB
 *  supports non-string jsonb values by serializing them and tracks if auth header present (later helps in preventing overriding with basic auth )
*/
static struct curl_slist*
build_headers(Jsonb *headers_jsonb,bool *has_auth_header)
{
    struct curl_slist *chunk = NULL;

    JsonbIterator *it;
    JsonbValue v;
    JsonbIteratorToken r;

    if (!headers_jsonb)
        return NULL;

    if (!JB_ROOT_IS_OBJECT(headers_jsonb))
        ereport(ERROR, (errmsg("headers must be a JSON object")));

    it = JsonbIteratorInit(&headers_jsonb->root);

    while ((r = JsonbIteratorNext(&it, &v, false)) != WJB_DONE)
    {
        if (r == WJB_KEY)
        {
            char *key = pnstrdup(v.val.string.val, v.val.string.len);

            r = JsonbIteratorNext(&it, &v, false);  // get VALUE

            if (r != WJB_VALUE)
            {
                pfree(key);
                continue;
            }

            char *val;
            /*
             *  fast path : string vals
             *  fallback : serialize JSON value into stirng
            */
            if (v.type == jbvString)
            {
                val = pnstrdup(v.val.string.val, v.val.string.len);
            }
            else
            {
                Jsonb *tmp = JsonbValueToJsonb(&v);

                StringInfoData buf;
                initStringInfo(&buf);

                JsonbToCString(&buf, &tmp->root, VARSIZE_ANY(tmp));
                val = pnstrdup(buf.data, buf.len);

                pfree(buf.data);
            }

            StringInfoData header;
            initStringInfo(&header);

            appendStringInfo(&header, "%s: %s", key, val);
            if (pg_strcasecmp(key, "Authorization") == 0)
                *has_auth_header = true;
            chunk = curl_slist_append(chunk, header.data);

            pfree(key);
            pfree(val);
            pfree(header.data);
        }
    }

    return chunk;
}

static Jsonb *
headers_map_to_jsonb(HTAB *htab)
{
    JsonbParseState *state = NULL;
    HASH_SEQ_STATUS status;
    header_entry *entry;
    JsonbValue *result;
    
    pushJsonbValue(&state, WJB_BEGIN_OBJECT, NULL);

    hash_seq_init(&status, htab);

    while ((entry = hash_seq_search(&status)) != NULL)
    {
        JsonbValue key;
        key.type = jbvString;
        key.val.string.val = entry->key;
        key.val.string.len = strlen(entry->key);

        pushJsonbValue(&state, WJB_KEY, &key);

        if (list_length(entry->values) == 1)
        {
            char *val_str = linitial(entry->values);

            JsonbValue val;
            val.type = jbvString;
            val.val.string.val = val_str;
            val.val.string.len = strlen(val_str);

            pushJsonbValue(&state, WJB_VALUE, &val);
        }
        else
        {
            pushJsonbValue(&state, WJB_BEGIN_ARRAY, NULL);

            ListCell *lc;
            char *val_str ;
            foreach(lc, entry->values)
            {
                val_str = lfirst(lc);

                JsonbValue val;
                val.type = jbvString;
                val.val.string.val = val_str;
                val.val.string.len = strlen(val_str);

                pushJsonbValue(&state, WJB_ELEM, &val);
            }

            pushJsonbValue(&state, WJB_END_ARRAY, NULL);
        }
    }

    result = pushJsonbValue(&state, WJB_END_OBJECT, NULL);

    return JsonbValueToJsonb(result);
}

static HTAB *
parse_headers_to_map(char *raw_headers)
{
    HASHCTL ctl;
    HTAB *htab;

    memset(&ctl, 0, sizeof(ctl));
    ctl.keysize = 256;
    ctl.entrysize = sizeof(header_entry);

    htab = hash_create("headers map", 32, &ctl, HASH_ELEM | HASH_STRINGS);

    char *line;
    char *saveptr;

    for (line = strtok_r(raw_headers, "\r\n", &saveptr);
         line != NULL;
         line = strtok_r(NULL, "\r\n", &saveptr))
    {
        char *colon = strchr(line, ':');
        if (!colon)
            continue;

        *colon = '\0';

        char *k = line;
        char *v = colon + 1;

        while (*v == ' ' || *v == '\t')
            v++;

        bool found;
        // header_entry *entry =
        //     hash_search(htab, k, HASH_ENTER, &found);

        // if (!found)
        //     entry->values = NIL;

        // entry->values = lappend(entry->values, pstrdup(v));
        header_entry *entry = hash_search(htab, k, HASH_ENTER, &found);

        if (!found)
        {
            strlcpy(entry->key, k, sizeof(entry->key));
            entry->values = NIL;
        }

        entry->values = lappend(entry->values, pstrdup(v));
    }

    return htab;
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
        pg_strcasecmp(method, "DELETE") == 0
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
            return true;
        default:
            return false;
    }
}

/*
 *  main http req execution 
 *      - builds headers (default + user)
 *      - conf curl
 *      - retry
 *      - enforce limits (timeout,size)
 *      - convert resp to https_response
 */
https_result*
https_execute(
    const char *url,const char *method,Jsonb *headers_jsonb,const char *req_body,
    const int timeout_override,
    const char *username,const char *password,
    int retries,int retry_delay_ms,double retry_backoff
)
{

    bool has_auth_header = false;
    bool user_has_auth = false;
    bool default_has_auth = false;
    
    CURLcode res ;// NULL ;
    long http_code = 0;

    struct curl_slist *curl_headers = NULL ;
    https_result *result ;

    struct curl_buffer response_body;
    struct curl_buffer response_headers;

    struct timespec start, end;
    int duration_ms = 0;
    int effective_timeout = https_timeout;//--new
    long curl_tls_version = CURL_SSLVERSION_DEFAULT;

    JsonbParseState *state = NULL;
    Jsonb *default_headers_jsonb = NULL;
    struct curl_slist *default_list = NULL;
    struct curl_slist *user_list = NULL;

    int attempt = 0;
    int delay = retry_delay_ms;


    JsonbValue key, val;

    CURL *curl = curl_easy_init();
    if (!curl)
        ereport(ERROR, (errmsg("Failed to init curl")));

    // parse default headers per req
    if (pg_https_default_headers && strlen(pg_https_default_headers) > 0)
    {
        default_headers_jsonb = DatumGetJsonbP(
            DirectFunctionCall1(jsonb_in,
                CStringGetDatum(pg_https_default_headers)));
    }

    // default_headers_jsonb = cached_default_headers;

    // append user headers to default headers
    default_list = build_headers(default_headers_jsonb, &default_has_auth);
    user_list    = build_headers(headers_jsonb, &user_has_auth);

    has_auth_header = user_has_auth || default_has_auth;
    
    if (default_list)
    {
        curl_headers = default_list;

        if (user_list)
        {
            struct curl_slist *tmp = default_list;
            while (tmp->next)
                tmp = tmp->next;

            tmp->next = user_list;
        }
    }
    else
    {
        curl_headers = user_list;
    }

    initStringInfo(&response_body.data);
    initStringInfo(&response_headers.data);

    clock_gettime(CLOCK_MONOTONIC, &start);

    // ---- curl config ----
    curl_easy_setopt(curl, CURLOPT_URL, url);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *) &response_body);

    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, (void *) &response_headers);

    // curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, https_connect_timeout);// GUC defined
    // curl_easy_setopt(curl, CURLOPT_TIMEOUT, https_timeout);// GUC defined

    if (timeout_override > 0)
        effective_timeout = timeout_override;

    // prevents long blocking inside db backend
    if (effective_timeout > 30)
        effective_timeout = 30;
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, effective_timeout);



    // TLS/ssl verion handling
    // curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);//deprecated ,vars added now
    if (https_tls_version == 12)
        curl_tls_version = CURL_SSLVERSION_TLSv1_2;
    else if (https_tls_version == 13)
        curl_tls_version = CURL_SSLVERSION_TLSv1_3;
    else if (https_tls_version != 0)
        ereport(ERROR,
            (errmsg("invalid tls_version: %d", https_tls_version)));

    /*verify peer */
    /* Safe defaults */
    // curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    // curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    // no more needed, can be adjusted from PG
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, https_verify_peer ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, https_verify_peer ? 2L : 0L);
    
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, curl_tls_version);


    /* HTTP/2 */
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);

    /* ALPN */
    curl_easy_setopt(curl, CURLOPT_SSL_ENABLE_ALPN, 1L);

    /* Compression, sys default */
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

    /* Interrupt + hang fix */
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

    /* prevent slow/stall/hanging conns */
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L); // bytes/sec
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, effective_timeout);   // seconds

    
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "pg_https/1.0");
    
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L); //libcurl may use signals
    
    /* ---- headers ---- */

    // curl_headers = build_headers(headers_jsonb,&has_auth_header);

    if (curl_headers)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, curl_headers);

    // apply basic auth only if no auth header 

    // if (username)// what if only username reqd
    // if (username && *username)//avoidinf empty strings
    if (!has_auth_header && username && *username)//if auth avail
    {
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
        curl_easy_setopt(curl, CURLOPT_USERNAME, username);

        if (password)
            curl_easy_setopt(curl, CURLOPT_PASSWORD, password);
    }

    //  method handling (GET, POST, PUT DELETE PATCH)
     
    // if (method && strcmp(method, "POST") == 0)
    // {
    //     curl_easy_setopt(curl, CURLOPT_POST, 1L);

    //     if (req_body)
    //         curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req_body);
    // }
    // else
    // {
    //     curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    // }
    if (!method)
        ereport(ERROR, (errmsg("HTTP method cannot be NULL")));

    // if (strcmp(method, "GET") == 0)
    if (pg_strcasecmp(method, "GET") == 0)
    {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    }
    else
    {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);

        // if (req_body)
        //     curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req_body);
        // explicitly set size because payload may contain binary or null bytes
        if (req_body)//curl assumes null terminated string,breaks for binary payloads,josn with embedded nulls
        {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req_body);
            // curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, strlen(req_body));
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long) strlen(req_body));
        }
    }

    // retry loop with perform request & check res
    // res = curl_easy_perform(curl); // libcurl request // adding retry logic
    for (attempt = 0; attempt <= retries; attempt++)
    {
        CHECK_FOR_INTERRUPTS();

        res = curl_easy_perform(curl);

        if (res == CURLE_OK)
        {
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

            if (http_code < 500)
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

        pg_usleep(delay * 1000);
        delay = (int)(delay * retry_backoff);
    }
    // if (res != CURLE_OK)
    // {
    //     ereport(ERROR,
    //         (errmsg("curl request failed: %s",
    //                 curl_easy_strerror(res))));
    // }
    // if (!res)
    //     ereport(ERROR, (errmsg("https_execute returned NULL")));
    // write to abort due to size limit
    if (res == CURLE_WRITE_ERROR)
    {
        if (curl_headers)
            curl_slist_free_all(curl_headers);

        curl_easy_cleanup(curl);

        ereport(ERROR, (errmsg("response too large")));
    
    }
    // return struct err instead of simple err
    if (res != CURLE_OK)
    {
        result = palloc(sizeof(https_result));

        result->status = 0;
        result->body = psprintf("curl error: %s", curl_easy_strerror(res));
        result->headers = NULL;
        result->duration_ms = 0;
        result->response_bytes = 0;

        if (curl_headers)
            curl_slist_free_all(curl_headers);

        curl_easy_cleanup(curl);

        return result;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    clock_gettime(CLOCK_MONOTONIC, &end);//stats

    duration_ms =
        (end.tv_sec - start.tv_sec) * 1000 +
        (end.tv_nsec - start.tv_nsec) / 1000000;

    /* ---- build result ---- */
    result = palloc(sizeof(https_result));// creating result struct 

    //populating the result struct
    result->status = (int) http_code;

    // result->body = pstrdup(response_body.data.data); // user len aware copy, resp may contain binary/null
    result->body = pnstrdup(response_body.data.data, response_body.data.len);

    // found headers stored as JSONB with raw field
    // result->headers = pstrdup(response_headers.data.data);
    // char *headers_copy = pstrdup(response_headers.data.data);
    char *headers_copy = palloc(response_headers.data.len + 1);
    memcpy(headers_copy, response_headers.data.data, response_headers.data.len);
    headers_copy[response_headers.data.len] = '\0';
    
    // HTAB *map = parse_headers_to_map(headers_copy);
    // result->headers = headers_map_to_jsonb(map);
    // hash_destroy(map);
    pushJsonbValue(&state, WJB_BEGIN_OBJECT, NULL);

    key.type = jbvString;
    key.val.string.val = "raw";
    key.val.string.len = 3;

    val.type = jbvString;
    // val.val.string.val = headers_copy;//using pfree
    // val.val.string.val = pnstrdup(headers_copy, strlen(headers_copy));//for safety
    val.val.string.val = pnstrdup(headers_copy, strlen(headers_copy));
    val.val.string.len = strlen(headers_copy);
    val.val.string.len = strlen(headers_copy);

    pushJsonbValue(&state, WJB_KEY, &key);
    pushJsonbValue(&state, WJB_VALUE, &val);

    JsonbValue *jb_res = pushJsonbValue(&state, WJB_END_OBJECT, NULL);

    result->headers = JsonbValueToJsonb(jb_res);

    result->duration_ms = duration_ms;
    result->response_bytes = response_body.data.len;
    result->curl_error_code = res;
    result->error_message = pstrdup(curl_easy_strerror(res));

    /* ---- cleanup/free mem ---- */
    if (curl_headers)
        curl_slist_free_all(curl_headers);
    pfree(headers_copy);
    curl_easy_cleanup(curl);

    return result;
}