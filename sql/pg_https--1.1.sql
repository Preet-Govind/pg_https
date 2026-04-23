DO $$
BEGIN
    PERFORM 1;
END $$;

-- drop extension if exists  pg_https cascade ; 

-- create extension pg_https ;

-- drop schema if exists requests;

create schema if not exists requests ;

set pg_https.timeout = 20;
set pg_https.connect_timeout = 30;

set pg_https.tls_version = 13;--tls 1.3 , use 12 for 1.2
set pg_https.verify_peer = true; -- false validate ssl cert
set pg_https.tcp_keepalive = 0; 

set pg_https.pg_https_http_version = 2 ;
-- set pg_https.has_auth_header = false; // breaks per request correctness

-- SET pg_https.default_headers = '{"User-Agent": "pg_https/1.1"}';
-- SET pg_https.default_headers = '{"Content-Type":"application/json"}';

SET pg_https.default_headers = '{"User-Agent": "pg_https/1.1","Content-Type":"application/json"}';

set pg_https.max_response_size = 10485760 ; -- 10 MB

--------------------------------------------------
-- create type http_response as (
--     status int,
--     -- headers text,
--     headers jsonb, body text);

CREATE TYPE http_response AS (
    status int,
    headers jsonb,body text,
    duration_ms int,response_bytes int,
    curl_error_code int,error_message text
);
-- create function rest_request(
--     method text,
--     url text
-- )
-- returns http_response
-- as 'module_pathname', 'rest_request'
-- LANGUAGE C STRICT;

-- ---
create or replace function requests.rest_request(
    method text,
    url text,
    headers jsonb default null, body text default null,
    timeout int default null,
    username text default null, password text default null 
    ,retries int default 0,retry_delay_ms int default 300,retry_backoff float default 2.0
    ,cancel_mode int default 0 --,tcp_keepalive int default true -- placing keepalive as GUC instead 
)
-- returns here won't work
-- RETURNS TABLE (
--     status int,
--     headers jsonb,
--     body text,
--     duration_ms int,
--     response_bytes int
-- )
returns http_response
AS 'MODULE_PATHNAME', 'rest_request'
LANGUAGE C;

--------------------

create OR REPLACE function requests.auth_bearer(token text)
returns jsonb as $$
    select jsonb_build_object('Authorization','Bearer'||token);
$$ LANGUAGE sql IMMUTABLE;


create or replace function requests.get_req(    url text,
    headers jsonb default null, body text default null,
    timeout int default null,
    username text default null, password text default null -- new
    )
returns http_response  AS
$$ SELECT * FROM requests.rest_request('GET', url,headers,body,timeout,username,password);
$$ LANGUAGE sql IMMUTABLE;

create or replace function requests.post_req(    url text,
    headers jsonb default null, body text default null,
    timeout int default null,
    username text default null, password text default null -- new
    )
returns http_response AS 
$$ SELECT * FROM requests.rest_request('POST', url,headers,body,timeout,username,password);
$$ LANGUAGE sql IMMUTABLE;