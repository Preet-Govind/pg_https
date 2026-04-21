#ifndef HTTPS_CORE_H
#define HTTPS_CORE_H

#include "utils/jsonb.h"

#include "utils/hsearch.h"

/*
 * represents a single http header key with possible multiple vals
 *  used when parsing raw headers into struct map
 *      Set-Cookie: a=1
 *      Set-Cookie: a=1
 *      key =" Set-Cookie" ,  values=["a=1","b=1"]
*/


typedef struct header_entry
{
    char key[256]; //fixed size key buf , safe for typical header names
    List *values; // list of char* values , pg list
} header_entry;


// https results / response returned from https_execute()

typedef struct
{
int status;
char *body;
// char *headers;
Jsonb *headers; // currently contains raw but struct allows formating
int duration_ms; // total req time ms
int response_bytes; // size of resp body
int curl_error_code; // libcurl err code ,CURLE_*
char *error_message; // err
} https_result;


// https execute params
// https_result* https_execute(const char *url); // deprecated , as now more params added
/*
 * Core HTTP execution API
 *
 * notes:
 *  - All pointers are expected to be valid C strings unless NULL
 *  - headers: JSONB object (key → value)
 *  - timeout_override: per-request override (-1 = use GUC default)
 *
 * retry behavior:
 *  - retries: max retry attempts
 *  - retry_delay_ms: initial delay between retries
 *  - retry_backoff: multiplier for exponential backoff
 */

https_result* https_execute(
    const char *url,const char *method,Jsonb *headers,const char *req_body,int req_body_len,
    int timeout_override ,
    const char *username,const char *password
    ,int retries,int retry_delay_ms,double retry_backoff
    ,int cancel_mode
);



// shared config variables , defiened via pg's GUC, globally accessible across the extension
// timeout 
extern int https_timeout ; // req timeout sec
extern int https_connect_timeout ; // conn timeout sec
extern int https_tls_version; // TLS version selector
extern bool https_verify_peer; // SSL certificate verfication 

extern int pg_https_tcp_keepalive;//keep alive tcp 

typedef enum
{
    HTTPS_CANCEL_ABORT = 0,
    HTTPS_CANCEL_WAIT  = 1
} https_cancel_mode;

/* 
 * mem :
 *  - All fields are allocated in current memory context
 *  - Caller (Postgres function) does not free manually
 */
 
#endif