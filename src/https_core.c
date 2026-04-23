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

// #ifndef CURLOPT_TCP_KEEPCNT  // use curl 8.14
// #define CURLOPT_TCP_KEEPCNT 3l
// #endif
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
extern char *pg_https_ca_file;

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

typedef struct https_curl_ctx
{
    https_cancel_mode cancel_mode;
} https_curl_ctx;

// merge default and user list safely
struct curl_slist *merge_headers(struct curl_slist *default_list, struct curl_slist *user_list)
{
    if (!default_list) return user_list ;
    if(!user_list) return default_list ;
    struct curl_slist *tmp = default_list;// tmp starts at the head of default list 
    while(tmp->next) //moves tmp to the last node in default list
        tmp = tmp->next ;
    tmp->next = user_list ; // appends user_list 
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
    // CHECK_FOR_INTERRUPTS();  // allow cancel / statement_timeout
    // return 0;
    /*
     * not checking INTR here , does a longjmp() from libcurl , which skips curls internal clean up and causing leaking the handle ssl ctx and socket
     * instead , sig libcurl to abort by non-0 , curl_easy_perform will then ret CURLE_ABORTED_BY_CALLBACK and then we will check for INTR
    */
    // if (QueryCancelPending || ProcDiePending)
    //     return 1;
    // return 0;
    if (QueryCancelPending || ProcDiePending)
    {
        https_curl_ctx *ctx = (https_curl_ctx *) clientp; // clientp seems garbage here

        if (ctx->cancel_mode == HTTPS_CANCEL_ABORT) return 1;  // abort curl, hope for gracefully

        if (ctx->cancel_mode == HTTPS_CANCEL_WAIT)  return 0;  // ignore cancel, continue request, assuming ,won't need it
    }
    return 0;
}

/* 
 * write callback, response body
 *  - appends incoming chunks into buf + enforces max resp size 
 *  returning 0 signals libcurl to abort the transfer
 */
// unreachable url unsafe
// static size_t
// write_callback(void *contents, size_t size, size_t nmemb, void *userp)
// {
//     size_t realsize = size * nmemb;
//     struct curl_buffer *mem = (struct curl_buffer *) userp;
    
//     //what if the size too big
//     // if (mem->data.len + realsize > MAX_RESPONSE_SIZE)
//     if (mem->data.len + realsize > pg_https_max_response_size)
//         return 0;  // abort transfer
//     // if (res == CURLE_WRITE_ERROR)
//     //     ereport(ERROR, (errmsg("response too large")));

//     appendBinaryStringInfo(&mem->data, contents, realsize);
//     return realsize;
// }

static size_t
write_callback(void *contents, size_t size, size_t nmemb, void *userp)
{
    if (!userp || !contents)
        return size*nmemb ;
        // return 0;
    
    struct curl_buffer *mem = (struct curl_buffer *) userp;

    if (!mem)
        return 0;
    
    // overflow protection
    if (nmemb != 0 && size > SIZE_MAX / nmemb)
        return 0;

    size_t realsize = size * nmemb;

    // enforce max response size
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
    if (!buffer || !userdata)
        return 0;
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
    if (has_auth_header)
        *has_auth_header = false;//no header may cause err
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

// deprecated headers_map_to_jsonb
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
//

// header name validations
// HTTP spec RFC says invalid names , only A-Z a-z 0-9 !#$%&'*+-.^_`|~ allowed
static bool
is_valid_header_name(const char *k)
{
    for (const char *p = k ; *p; p++){
        if(!(
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

// header val validations
static bool
is_valid_header_value(const char *v)
{
    for(const char *p=v; *p; p++)
    {
        if (*p == '\r' || *p == '\n' )
            return false;
    }
    return true;
}

// used for header parsing , key val pairing
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
        if (strlen(k) == 0 || strlen(k) > 255)
            ereport(ERROR, (errmsg("invalid header name length")));

        if (!is_valid_header_name(k))
            ereport(ERROR, (errmsg("invalid header name")));

        if (!is_valid_header_value(v))
            ereport(ERROR, (errmsg("invalid header value")));
        // header_entry *entry =
        //     hash_search(htab, k, HASH_ENTER, &found);

        // if (!found)
        //     entry->values = NIL;

        // entry->values = lappend(entry->values, pstrdup(v));
        // header_entry *entry = hash_search(htab, k, HASH_ENTER, &found);//header keys needs to be normalized , i myself had done this mistake while querying so need to add this explicitly here
        char lower_key[256];

        strlcpy(lower_key,k,sizeof(lower_key));
        // pg_strtolower(lower_key);
        // pg_strtolower(lower_key, lower_key, strlen(lower_key));// api = pg_strtolower(char *dst, const char *src, size_t len)
        for (char *p = lower_key; *p; p++)
            *p = pg_tolower((unsigned char)*p);
        header_entry *entry = hash_search(htab,lower_key,HASH_ENTER,&found);//case insensittive hash look up , helps in aligning with HTTP spec
        if (!found)
        {
            // strlcpy(entry->key, k, sizeof(entry->key));
            if (strlen(k) >= sizeof(entry->key))
                ereport(ERROR, (errmsg("header name too long")));
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
        pg_strcasecmp(method, "DELETE") == 0 ||
        pg_strcasecmp(method, "OPTIONS") == 0   /* got in RFC */
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
sleepy(long delay , long jitter)
{
    long remaining_us = (long)(delay + jitter) * 1000L;// us = micro sec
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
 *      - builds headers (default + user)
 *      - conf curl
 *      - retry
 *      - enforce limits (timeout,size)
 *      - convert resp to https_response
 *  in v1.0 
 *      - mainly used palloc, pnstrdup and stringInfo func , so curr mem context gets used , usually fcinfo--> flinfo --> fn_next or i guess per call context , which may accumulate more mem if n calls , where n is very large
 *      - so allocating temp working mem in a short lived context which gets freed after each req call , using MemoryContext 
 */
https_result*
https_execute(
    const char *url,const char *method,Jsonb *headers_jsonb,const char *req_body,int req_body_len,
    const int timeout_override,
    const char *username,const char *password,
    int retries,int retry_delay_ms,double retry_backoff
    ,int cancel_mode 
)
{
    // mem context 
    MemoryContext old_ctx ;
    MemoryContext req_ctx ;
    req_ctx = AllocSetContextCreate(CurrentMemoryContext, "pg_https req ctx", ALLOCSET_DEFAULT_SIZES );
    old_ctx = MemoryContextSwitchTo(req_ctx) ; // so basically what i think here is , all palloc StringInfo jsonb would now be into req_ctx - including curl slist strings

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
    int effective_timeout = https_timeout; //
    long curl_tls_version = CURL_SSLVERSION_DEFAULT;

    JsonbParseState *state = NULL;
    Jsonb *default_headers_jsonb = NULL;
    struct curl_slist *default_list = NULL;
    struct curl_slist *user_list = NULL;

    int attempt = 0;
    int delay = retry_delay_ms;

    char *headers_copy = NULL ; 

    JsonbValue key, val;

    https_curl_ctx ctx;
    ctx.cancel_mode = cancel_mode;
    int tcp_keepalive = pg_https_tcp_keepalive;//--new

    CURL *curl = curl_easy_init();
    if (!curl)
    {
        MemoryContextSwitchTo(old_ctx);
        MemoryContextDelete(req_ctx);
        ereport(ERROR, (errmsg("Failed to init curl")));
    }
    PG_TRY();
    {
        // curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L); // uncomment to see verbose
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
        // moving this logic to merge_headers()    
        // if (default_list)
        // {
        //     curl_headers = default_list;

        //     if (user_list)
        //     {
        //         struct curl_slist *tmp = default_list;
        //         while (tmp->next)
        //             tmp = tmp->next;

        //         tmp->next = user_list;
        //     }
        // }
        // else
        // {
        //     curl_headers = user_list;
        // }
        curl_headers = merge_headers(default_list,user_list);
        // default_list = NULL; // dumb ways to deal with ptr
        // user_list = NULL;

        initStringInfo(&response_body.data);
        initStringInfo(&response_headers.data);

        clock_gettime(CLOCK_MONOTONIC, &start);

        // ---- curl config ----
        if (!url || strlen(url) == 0)
            ereport(ERROR, (errmsg("URL cannot be empty")));
        curl_easy_setopt(curl, CURLOPT_URL, url);


        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *) &response_body);

        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, (void *) &response_headers);

        // curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, https_connect_timeout);// GUC defined
        // curl_easy_setopt(curl, CURLOPT_TIMEOUT, https_timeout);// GUC defined

        // if (timeout_override > 0)   effective_timeout = timeout_override;
        effective_timeout = (timeout_override > 0) ? timeout_override : https_timeout;// atleast https_timeout is maintained with effective timeout


        // prevents long blocking inside db backend
        // if (effective_timeout > 30)
        // {
        //     ereport(WARNING, (errmsg("pg_https: timeout clamped to 30s (requested: %ds)", effective_timeout)));
        //     effective_timeout = 30;
        // }
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
        // curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, https_verify_peer ? 1L : 0L);
        // curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, https_verify_peer ? 2L : 0L); // what if i split VERIFYHOST and VERIFYPEER

        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER,https_verify_peer ? 1L : 0L);

        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST,https_verify_peer ? 2L : 0L);
        
        curl_easy_setopt(curl, CURLOPT_SSLVERSION, curl_tls_version);

        // ca
        if (pg_https_ca_file && strlen(pg_https_ca_file) > 0)
        {
            curl_easy_setopt(curl, CURLOPT_CAINFO, pg_https_ca_file);
        }
        /* HTTP/2 */
        // curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
        curl_easy_setopt(curl, CURLOPT_HTTP_VERSION,(pg_https_http_version == 2)? CURL_HTTP_VERSION_2TLS: CURL_HTTP_VERSION_1_1);

        /* ALPN */
        // curl_easy_setopt(curl, CURLOPT_SSL_ENABLE_ALPN, 1L);
        /* only enable ALPN when HTTP/2 is explicitly requested */
        if (pg_https_http_version == 2)
            curl_easy_setopt(curl, CURLOPT_SSL_ENABLE_ALPN, 1L);

        /* Compression, sys default */
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");


        /* Interrupt + hang fix */
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);// for intr
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

        /* keep-alive idle time to 120 seconds */
        if (tcp_keepalive >0 )
        {
            /* enable TCP keep-alive for this transfer */
            curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);

            /* set keep-alive idle time to tcp_keepalive seconds */
            curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, tcp_keepalive);
            /* interval time between keep-alive probes: 60 seconds */
            curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, 60L);
        
            /* maximum number of keep-alive probes: 3 */
            curl_easy_setopt(curl, CURLOPT_TCP_KEEPCNT, 3L);
        }

        /* prevent slow/stall/hanging conns */
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L); // bytes/sec
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, https_connect_timeout);   // seconds

        
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "pg_https/1.1");
        
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L); //libcurl may use signals
        // may contribute to spikes in conns
        // /* this second transfer may not reuse the same connection */ 
        // curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 1L);
        // /* this transfer must use a new connection, not reuse an existing */
        // curl_easy_setopt(curl, CURLOPT_FRESH_CONNECT, 1L);
        
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
            if (req_body && req_body_len > 0 )//curl assumes null terminated string,breaks for binary payloads,josn with embedded nulls
            {
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req_body);
                // curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, strlen(req_body));
                // curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long) strlen(req_body));// taken req_body_len, passed in param
                // curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long) req_bod_len);// fix
                curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t) req_body_len);//avoids larger payloads
            }
        }

        // retry loop with perform request & check res
        // res = curl_easy_perform(curl); // libcurl request // adding retry logic
        
        int max_delay = 10000;  // 10 seconds cap , retry
        char errbuf[CURL_ERROR_SIZE];
        errbuf[0] = '\0';
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);

        for (attempt = 0; attempt <= retries; attempt++)
        {
            // response_body.data.len = 0;
            // response_headers.data.len = 0;
            if (attempt > 0)//just for debugging
            {
                ereport(LOG,
                    (errmsg("pg_https retry %d for %s %s",
                        attempt, method, url)));
            }
            CHECK_FOR_INTERRUPTS();
            // response_body.data.len = 0;
            // response_headers.data.len = 0; 
            //cleaner overwrite, i guess although i doubt StringInfo
            resetStringInfo(&response_body.data);
            resetStringInfo(&response_headers.data);
            http_code = 0;

            res = curl_easy_perform(curl);

            ereport(LOG, (errmsg("pg_https: curl : attempt %d ,result=%d (%s)",attempt, res, curl_easy_strerror(res))));
            
            
            if (res == CURLE_ABORTED_BY_CALLBACK)
            {
                MemoryContextSwitchTo(old_ctx);
                result = palloc0(sizeof(https_result));
                result->curl_error_code = CURLE_ABORTED_BY_CALLBACK;
                result->error_message = pstrdup("query cancelled");
                result->headers = DatumGetJsonbP(
                    DirectFunctionCall1(jsonb_in, CStringGetDatum("{}")));

                /* curl is done, goto cleanup will run curl_easy_cleanup properly */
                goto cleanup;
                /* After cleanup, CHECK_FOR_INTERRUPTS will throw the actual cancel error */
            }
            if (res == CURLE_OK)
            {
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            
                // if (http_code < 500)
                if (http_code < 500 &&
                        http_code != 408 &&
                        http_code != 429)
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

            // pg_usleep(delay * 1000);
            // delay = (int)(delay * retry_backoff);
            // int jitter = random() % 100;  // 0–99 ms jitter , check - pg gaurantees seeded random()  
            int jitter = pg_prng_uint32(&pg_global_prng_state) % 100;//srandom may return void
            // CHECK_FOR_INTERRUPTS();

            // pg_usleep((delay + jitter) * 1000); // delya 1st
            // delay = (int)(delay * retry_backoff);// inc delay for next attempt
            sleepy(delay,jitter);
            delay = (int)(delay*retry_backoff);
            if (delay > max_delay)
                delay = max_delay;
        }
    
        // write to abort due to size limit
        if (res == CURLE_WRITE_ERROR)
        {
            // MemoryContextSwitchTo(old_ctx);
            // if (curl_headers)
            //     curl_slist_free_all(curl_headers);

            // curl_easy_cleanup(curl);
            // ereport(ERROR, (errmsg("response too large"))); // i was doing this only earlier
            
            // now
            MemoryContextSwitchTo(old_ctx);
            result = palloc0(sizeof(https_result));
            result->status = 0;
            result->curl_error_code = res;
            result->error_message = pstrdup("response exceeded max_response_size limit");
            result->headers = DatumGetJsonbP(
                DirectFunctionCall1(jsonb_in, CStringGetDatum("{}")));
            goto cleanup;
        
        }
        // return struct err instead of simple err
        if (res != CURLE_OK)
        {
            MemoryContextSwitchTo(old_ctx);
            result = palloc(sizeof(https_result));

            memset(result,0,sizeof(https_result));

            result->status = 0;
            // result->body = psprintf("curl error: %s", curl_easy_strerror(res));//might be lossing error buf details
            result->body = psprintf("{\"error\":true,\"message\":\"%s\",\"detail\":\"%s\"}", curl_easy_strerror(res),errbuf[0] ? errbuf : "no detail");
            
            // result->headers = NULL;
            result->headers = DatumGetJsonbP(
                DirectFunctionCall1(jsonb_in, CStringGetDatum("{}"))
            );// always return json
            result->duration_ms = 0;
            result->response_bytes = 0;

            result->curl_error_code=res ;
            // result->error_message = pstrdup(result->body);


            // if (curl_headers)
            //     curl_slist_free_all(curl_headers);// in clean up 

            // curl_easy_cleanup(curl);
            // curl = NULL;
            goto cleanup;

            // return result;// i think its going to be unreachable
        }

        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        clock_gettime(CLOCK_MONOTONIC, &end);//stats

        duration_ms =
            (end.tv_sec - start.tv_sec) * 1000 +
            (end.tv_nsec - start.tv_nsec) / 1000000;

        /* ---- build result ---- */
        MemoryContextSwitchTo(old_ctx) ;// switch back to old context
        result = palloc(sizeof(https_result));// creating result struct 

        //populating the result struct
        result->status = (int) http_code;

        // result->body = pstrdup(response_body.data.data); // user len aware copy, resp may contain binary/null
        // result->body = pnstrdup(response_body.data.data, response_body.data.len);
        if (response_body.data.data && response_body.data.len > 0)
            result->body = pnstrdup(response_body.data.data, response_body.data.len);
        else
            result->body = pstrdup("");

        result->error_message = (res == CURLE_OK) ? NULL : psprintf("curl error (%d): %s | detail: %s",res,curl_easy_strerror(res),errbuf[0] ? errbuf : "no detail");
        // found headers stored as JSONB with raw field
        // result->headers = pstrdup(response_headers.data.data);
        // char *headers_copy = pstrdup(response_headers.data.data);

        // headers_copy = palloc(response_headers.data.len + 1);
        // memcpy(headers_copy, response_headers.data.data, response_headers.data.len);//data = NULL and len == 0 then it may contribute to crash
        // so 
        if (response_headers.data.len > 0 && response_headers.data.data)
        {
            headers_copy = palloc(response_headers.data.len + 1);
            memcpy(headers_copy, response_headers.data.data, response_headers.data.len);
            headers_copy[response_headers.data.len] = '\0';
        }
        else
        {
            headers_copy = pstrdup("");
        }
        // headers_copy[response_headers.data.len] = '\0';// can cause seg fault
        
        // HTAB *map = parse_headers_to_map(headers_copy);
        // result->headers = headers_map_to_jsonb(map);
        // hash_destroy(map);
        pushJsonbValue(&state, WJB_BEGIN_OBJECT, NULL);
    //pushJsonbValue expects state to be managed internally but if anythin fails or mem cntxt switches or partial exe then  , hence state=null
        
        key.type = jbvString;
        key.val.string.val = "raw";
        key.val.string.len = 3;

        val.type = jbvString;
        // val.val.string.val = headers_copy;//using pfree
        // val.val.string.val = pnstrdup(headers_copy, strlen(headers_copy));//for safety
        // val.val.string.len = strlen(headers_copy);
        // val.val.string.val = pnstrdup(headers_copy, response_headers.data.len);// if \0 contained in header , truncate using strlen() ---> mismatch btwn actual buffer and len 
        // val.val.string.len = response_headers.data.len;//does not help when url unreachable - check
        int hdr_len = response_headers.data.len; // c90 move declarations
        if(!headers_copy || hdr_len <=0)
        {
            // hdr_len = 0 ; // its ptr !!
            val.val.string.len = 0;
            val.val.string.val = "" ; 
        }
        else
        {
                val.val.string.val = pnstrdup(headers_copy, hdr_len);
                val.val.string.len = hdr_len;
        }
        // val.val.string.val = pnstrdup(headers_copy, hdr_len);
        // val.val.string.len = hdr_len;

        pushJsonbValue(&state, WJB_KEY, &key);
        pushJsonbValue(&state, WJB_VALUE, &val);

        JsonbValue *jb_res = pushJsonbValue(&state, WJB_END_OBJECT, NULL);
        if (!jb_res)
            ereport(ERROR, (errmsg("failed to build jsonb headers")));
        
        result->headers = JsonbValueToJsonb(jb_res);

        result->duration_ms = duration_ms;
        result->response_bytes = response_body.data.len;
        result->curl_error_code = res;
        // result->error_message = pstrdup(curl_easy_strerror(res));//no context
        // result->error_message = psprintf("curl error (%d): %s",res,curl_easy_strerror(res)); // added context


        goto cleanup;
        /* ---- cleanup/free mem ---- */
        cleanup:
            // MemoryContextSwitchTo(old_ctx);
            if (curl_headers)
                curl_slist_free_all(curl_headers);
            if (headers_copy)
                pfree(headers_copy);
        
            // curl_easy_cleanup(curl);
            if (curl)
                curl_easy_cleanup(curl);
            // NOW using mem context        
            // if (response_body.data.data)
            //     pfree(response_body.data.data);

            // if (response_headers.data.data)
            //     pfree(response_headers.data.data);

            // if(default_headers_jsonb)
            // pfree(default_headers_jsonb);//freeing a mem which pg expects to exists might be making pg recovery mode , SIGSEGV
            
            MemoryContextDelete(req_ctx);// context clean up 
            // CHECK_FOR_INTERRUPTS();
            
        }
        PG_CATCH();
        {
            if (curl)   curl_easy_cleanup(curl);
            if (curl_headers)   curl_slist_free_all(curl_headers);
            MemoryContextSwitchTo(old_ctx);
            MemoryContextDelete(req_ctx);
            PG_RE_THROW();
        }
        PG_END_TRY();

        CHECK_FOR_INTERRUPTS(); 
        return result;
}
