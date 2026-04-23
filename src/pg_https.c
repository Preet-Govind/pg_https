#include "postgres.h"

#include "fmgr.h"
#include "utils/builtins.h"
// #include "catalog/pg_type.h"

#include "funcapi.h"        // get_call_result_type, TYPEFUNC_* // for composite return types
#include "access/htup_details.h"  // heap_form_tuple, HeapTuple // tuple construction
// #include "utils/lsyscache.h"
#include "utils/guc.h" // custom config - GUC
#include <curl/curl.h>

#include "https_core.h"

/*
 *  arg index mapping , kept explicit to avoid magic numbers and make changes less error prone
*/

#define ARG_METHOD 0
#define ARG_URL 1
#define ARG_HEADERS 2
#define ARG_BODY 3
#define ARG_TIMEOUT 4
#define ARG_USERNAME 5
#define ARG_PASSWORD 6
#define ARG_RETRIES 7
#define ARG_RETRY_DELAY 8
#define ARG_RETRY_BACKOFF 9
#define ARG_CANCEL_MODE 10
// #define ARG_TCP_KEEPALIVE 11 // INT

PG_MODULE_MAGIC;

/*--- -- GUC backed vars  -- ---*/
int https_timeout = 10;
int https_connect_timeout = 5;

/*
 *  libcurl must be init once per process
 *  PG may load/unload extension multiple times so we gaurd this explicitly
*/
static bool curl_initialized = false;

int https_tls_version = 0;
bool https_verify_peer = true;

int pg_https_max_response_size = 10485760; // 10MB default


char *pg_https_default_headers = NULL;

char *pg_https_ca_file = NULL;

int pg_https_tcp_keepalive = 120 ; // sec default

int pg_https_http_version = 0; 

extern void init_default_headers(void);


/*
 *  extension init hook
 *      - register custom pg config var
 *      - init libcurl once per process
*/
static bool
check_hook_tls_version(int *newval, void **extra, GucSource source)
{
    if (*newval != 0 && *newval != 12 && *newval != 13)
        return false;
    return true;
}

void _PG_init(void)
{
    // init_default_headers();// lazy init in https_execute()
    // curl_global_init(CURL_GLOBAL_DEFAULT);
    if (!curl_initialized)// once per process
    {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        curl_initialized = true;
    }

    /* ---- Timeouts ---- */
    DefineCustomIntVariable(
        "pg_https.timeout",
        "HTTP request timeout (seconds)",
        NULL,
        &https_timeout,
        10,     /* default */
        1,      /* min */
        36000,    /* max */
        PGC_USERSET,
        0,
        NULL,
        NULL,
        NULL
    );

    DefineCustomIntVariable(
        "pg_https.connect_timeout",
        "HTTP connect timeout (seconds)",
        NULL,
        &https_connect_timeout,
        5,
        1,
        360,
        PGC_USERSET,
        0,
        NULL,
        NULL,
        NULL
    );
      /* ---- TLS version and behaviour ---- */
    DefineCustomIntVariable(
        "pg_https.tls_version",
        "TLS version (0=default, 12=TLS1.2, 13=TLS1.3)",
        NULL,
        &https_tls_version,
        0,
        0,
        13,
        PGC_USERSET,
        0,
        check_hook_tls_version,
        NULL,
        NULL
    );

    /* ---- SSL verification ---- */
    DefineCustomBoolVariable(
        "pg_https.verify_peer",
        "Verify SSL certificates",
        NULL,
        &https_verify_peer,
        true,
        PGC_USERSET,
        0,
        NULL,
        NULL,
        NULL
    );

    /* ---- default headers stored as json string and parsed later during req execution (lazy init) ---- */
    DefineCustomStringVariable
    (
        "pg_https.default_headers",
        "Default HTTP headers (JSON)",
        NULL,
        &pg_https_default_headers,
        "",
        PGC_USERSET,
        0,
        NULL,
        NULL,
        NULL
    );
    /* ---- hard safety cap to avoid unbounded mem usage ---- */
    DefineCustomIntVariable(
        "pg_https.max_response_size",
        "Max HTTP response size",
        NULL,
        &pg_https_max_response_size,
        10485760,
        1024,
        104857600,
        PGC_USERSET,
        0,
        NULL,
        NULL,
        NULL
    );

    DefineCustomStringVariable(
        "pg_https.ca_file",
        "Custom CA bundle path",
        NULL,
        &pg_https_ca_file,
        "",
        PGC_USERSET,
        0,
        NULL,
        NULL,
        NULL
    );
 /* ---- tcp keep alive  ---- */
    DefineCustomIntVariable(
        "pg_https.tcp_keepalive",
        "Max tcp keep alive sec",
        NULL,
        &pg_https_tcp_keepalive,
        120, /* default*/
        0,/* min */ // curl default
        300, /* max */ //intentionally kept 
        PGC_USERSET,
        0,
        NULL,
        NULL,
        NULL
    );

    DefineCustomIntVariable(
    "pg_https.http_version",
    "HTTP version (0=1.1, 2=HTTP/2)",
    NULL,
    &pg_https_http_version,
    0, 0, 2,
    PGC_USERSET, 0, NULL, NULL, NULL
);
}


PG_FUNCTION_INFO_V1(rest_request);

/*
 *  main sql entry point ; execute http req and returns composite response
*/

Datum rest_request(PG_FUNCTION_ARGS)
{
    /* ---- inputs ---- */
    char *url = NULL;
    char *method = NULL;
    char *body = NULL;

    text *method_text;
    text *url_text;
    Jsonb *headers_jsonb = NULL;

    int timeout_override = -1;

    https_result *res = NULL;

    TupleDesc tupdesc;
    HeapTuple tuple;

    /*
    *   result layout , should be matching with http_response struct
    *   [status, headers, body, duration_ms, bytes, curl_code, err_message ]
    */
    Datum values[7];
    bool nulls[7] = {false, false, false, false, false, false, false};
    
    char *username = NULL;
    char *password = NULL;
    
    int retries = 0;
    int retry_delay_ms = 100;
    double retry_backoff = 2.0;
    
    int cancel_mode = 0; // default = abort
    
    // body valena fix
    
    int body_len = 0;

    /* ---- reqd args ---- */
    if (PG_ARGISNULL(0))
        ereport(ERROR, (errmsg("method cannot be NULL")));

    if (PG_ARGISNULL(1))
        ereport(ERROR, (errmsg("URL cannot be NULL")));

    /* ---- Extract and normalize args ---- */
    method_text = PG_GETARG_TEXT_PP(0);//_PP = unpacked/detoasted safely;  _P packed
    url_text    = PG_GETARG_TEXT_PP(1);

    method = text_to_cstring(method_text);
    /*
    *   normalize rest method to upper case once here as well , same done in the https_requests as well using pg_strcasecmp func
    */
    for (char *p = method; *p; p++)
        *p = pg_toupper((unsigned char) *p);//just in case pg_strcasecmp fails

    url    = text_to_cstring(url_text);
    
    if(url == NULL || strlen(url) == 0 )
        ereport(ERROR, (errmsg("URL cannot be NULL")));
    
    // optional params
    if (!https_verify_peer)
    {
        ereport(WARNING,
            (errmsg("pg_https: SSL verification is disabled (insecure)")));
    }
    if (PG_NARGS() > 2 && !PG_ARGISNULL(2))
        headers_jsonb = PG_GETARG_JSONB_P(2);

    if (PG_NARGS() > 3 && !PG_ARGISNULL(3))
    {
        // body = text_to_cstring(PG_GETARG_TEXT_PP(3));// it seems broken, text_to_string() forces null terminated string, strlen() stops at first \0
        // so if req body contains binary data/json with \0 or compressed payloads , this might be truncated the request
        // so replacing it by
        text * body_text = PG_GETARG_TEXT_PP(3);
        body_len = VARSIZE_ANY_EXHDR(body_text);//returns size of var len (varlena) data val excluding its header, pg's
        body = VARDATA_ANY(body_text); // zero copy , faster safe within func life
    }

    // timeout 
    // if (PG_NARGS() > 4 && !PG_ARGISNULL(4)) // earlier when arg index mapping was not done
    //     timeout_override = PG_GETARG_INT32(4);
    if (PG_NARGS() > ARG_TIMEOUT && !PG_ARGISNULL(ARG_TIMEOUT)) // now as index mapping is done, ARG_TIMEOUT
        timeout_override = PG_GETARG_INT32(ARG_TIMEOUT);

    // user CREDS
    if (PG_NARGS() > 5 && !PG_ARGISNULL(5))
        username = text_to_cstring(PG_GETARG_TEXT_PP(5));

    if (PG_NARGS() > 6 && !PG_ARGISNULL(6))
        password = text_to_cstring(PG_GETARG_TEXT_PP(6));

    // retry params
    if (PG_NARGS() > ARG_RETRIES && !PG_ARGISNULL(ARG_RETRIES))
        retries = PG_GETARG_INT32(ARG_RETRIES);

    if (PG_NARGS() > ARG_RETRY_DELAY && !PG_ARGISNULL(ARG_RETRY_DELAY))
        retry_delay_ms = PG_GETARG_INT32(ARG_RETRY_DELAY);

    if (PG_NARGS() > ARG_RETRY_BACKOFF && !PG_ARGISNULL(ARG_RETRY_BACKOFF))
        retry_backoff = PG_GETARG_FLOAT8(ARG_RETRY_BACKOFF);

    if (PG_NARGS() > ARG_CANCEL_MODE && !PG_ARGISNULL(ARG_CANCEL_MODE)) cancel_mode = PG_GETARG_INT32(ARG_CANCEL_MODE);

    // if (PG_NARGS() > ARG_TCP_KEEPALIVE && !PG_ARGISNULL(ARG_TCP_KEEPALIVE)) tcp_keepalive = PG_GETARG_INT32(ARG_TCP_KEEPALIVE);


    /* ---- Clamp retry params , safety - to prevent abuse ---- */
    if (retries < 0)
        retries = 0;
    if (retries > 10)
        retries = 10;

    if (retry_delay_ms < 10)
        retry_delay_ms = 10;
    if (retry_delay_ms > 5000)
        retry_delay_ms = 5000;

    if (retry_backoff < 1.0)
        retry_backoff = 1.0;
    if (retry_backoff > 5.0)
        retry_backoff = 5.0;

    /* ---- execute HTTP req ---- */
    // res = https_execute(url, method, headers_jsonb, body);// deprecated as now timeout param is also taken
    // res = https_execute(url, method, headers_jsonb, body, timeout_override);//creds added
    // res = https_execute(url, method, headers_jsonb, bod/y, timeout_override, username,password);// adding retry logic
    res = https_execute(
                url, method, headers_jsonb, body,body_len,
                timeout_override, 
                username, password,
                retries, retry_delay_ms, retry_backoff
                ,cancel_mode
            );
/* https_execute(
    const char *url,const char *method,Jsonb *headers_jsonb,const char *req_body,int req_body_len,
    const int timeout_override,
    const char *username,const char *password,
    int retries,int retry_delay_ms,double retry_backoff
) */

    /* ---- Build response , what if its null , hence validate---- */
    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        ereport(ERROR, (errmsg("return type must be composite")));

    if (!res)
        ereport(ERROR, (errmsg("internal error: null response")));
    
    /* ---- logging slow or failing req , keeping it simple and light ---- */
    if (res->duration_ms > 1000 || res->status >= 400 || res->curl_error_code != 0)
        ereport(LOG,
            (errmsg("pg_https: %s %s --> %d (%d ms, %d bytes)",
                method,
                url,
                res->status,
                res->duration_ms,
                res->response_bytes)));


    BlessTupleDesc(tupdesc);

    // as popluated in result struct in pg_https.c , now formating for sql layer 
    values[0] = Int32GetDatum(res->status);
    // values[1] = CStringGetTextDatum(res->headers);// no, null response may cause err
    if (res->headers)//INCASE NULL RESPONSE recieved fix
        // values[1] = JsonbGetDatum(res->headers);// func not used by pg
        values[1] = JsonbPGetDatum(res->headers);//
    else
        nulls[1] = true;

    if (res->body)
        values[2] = CStringGetTextDatum(res->body);
    else
        nulls[2] = true;
    
    values[3] = Int32GetDatum(res->duration_ms);
    values[4] = Int32GetDatum(res->response_bytes);
    values[5] = Int32GetDatum(res->curl_error_code);

    if (res->error_message)
        values[6] = CStringGetTextDatum(res->error_message);
    else
        nulls[6] = true;

    tuple = heap_form_tuple(tupdesc, values, nulls);

    PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}