create schema if not exists requests;

CREATE TYPE http_response AS (
    status int,
    headers jsonb,
    body text,
    duration_ms int,
    response_bytes int,
    curl_error_code int,
    error_message text
);

CREATE OR REPLACE FUNCTION requests.rest_request(
    method text,
    url text,
    headers jsonb default null,
    body text default null,
    timeout int default null,
    username text default null,
    password text default null,
    retries int default 0,
    retry_delay_ms int default 300,
    retry_backoff float default 2.0,
    cancel_mode int default 0
)
RETURNS http_response
AS 'MODULE_PATHNAME', 'rest_request'
LANGUAGE C;

--------------------

CREATE OR REPLACE FUNCTION requests.auth_bearer(token text)
RETURNS jsonb AS $$
    SELECT jsonb_build_object('Authorization', 'Bearer ' || token);
$$ LANGUAGE sql IMMUTABLE;

/* GET Wrapper */
CREATE OR REPLACE FUNCTION requests.get_req(
    url text,
    headers jsonb default null,
    body text default null,
    timeout int default null,
    username text default null,
    password text default null
)
RETURNS http_response AS $$
    SELECT * FROM requests.rest_request('GET', url, headers, body, timeout, username, password);
$$ LANGUAGE sql;

CREATE OR REPLACE FUNCTION requests.get_request(
    url text,
    headers jsonb default null,
    body text default null,
    timeout int default null,
    username text default null,
    password text default null
)
RETURNS http_response AS $$
    SELECT * FROM requests.rest_request('GET', url, headers, body, timeout, username, password);
$$ LANGUAGE sql;

/* POST Wrapper */
CREATE OR REPLACE FUNCTION requests.post_req(
    url text,
    headers jsonb default null,
    body text default null,
    timeout int default null,
    username text default null,
    password text default null
)
RETURNS http_response AS $$
    SELECT * FROM requests.rest_request('POST', url, headers, body, timeout, username, password);
$$ LANGUAGE sql;

CREATE OR REPLACE FUNCTION requests.post_request(
    url text,
    headers jsonb default null,
    body text default null,
    timeout int default null,
    username text default null,
    password text default null
)
RETURNS http_response AS $$
    SELECT * FROM requests.rest_request('POST', url, headers, body, timeout, username, password);
$$ LANGUAGE sql;

/* PUT Wrapper */
CREATE OR REPLACE FUNCTION requests.put_req(
    url text,
    headers jsonb default null,
    body text default null,
    timeout int default null,
    username text default null,
    password text default null
)
RETURNS http_response AS $$
    SELECT * FROM requests.rest_request('PUT', url, headers, body, timeout, username, password);
$$ LANGUAGE sql;

CREATE OR REPLACE FUNCTION requests.put_request(
    url text,
    headers jsonb default null,
    body text default null,
    timeout int default null,
    username text default null,
    password text default null
)
RETURNS http_response AS $$
    SELECT * FROM requests.rest_request('PUT', url, headers, body, timeout, username, password);
$$ LANGUAGE sql;

/* DELETE Wrapper */
CREATE OR REPLACE FUNCTION requests.delete_req(
    url text,
    headers jsonb default null,
    body text default null,
    timeout int default null,
    username text default null,
    password text default null
)
RETURNS http_response AS $$
    SELECT * FROM requests.rest_request('DELETE', url, headers, body, timeout, username, password);
$$ LANGUAGE sql;

CREATE OR REPLACE FUNCTION requests.delete_request(
    url text,
    headers jsonb default null,
    body text default null,
    timeout int default null,
    username text default null,
    password text default null
)
RETURNS http_response AS $$
    SELECT * FROM requests.rest_request('DELETE', url, headers, body, timeout, username, password);
$$ LANGUAGE sql;