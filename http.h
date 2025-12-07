#pragma once

#include <stdio.h>

#define MAX_METHOD 16
#define MAX_URI 1024
#define MAX_VERSION 16
#define MAX_HEADER_VALUE 256

struct http_request
{
	char method[MAX_METHOD];
	char path[MAX_URI];
	char version[MAX_VERSION];
	char if_modified_since[MAX_HEADER_VALUE];
	char request_line[MAX_URI + MAX_METHOD + MAX_VERSION + 4];
};

struct http_response {
	int status_code;
	size_t content_len;
};

struct server_config;

enum HTTP_PARSE_RESULT
{
	HTTP_PARSE_OK = 0,
	HTTP_PARSE_INVALID_METHOD = -1,
	HTTP_PARSE_INVALID_URI = -2,
	HTTP_PARSE_INVALID_VERSION = -3,
	HTTP_PARSE_EOF = -4,
	HTTP_PARSE_LINE_FAILURE = -5,
};

enum HTTP_STATUS_CODE
{
	HTTP_STATUS_OK = 200,
	HTTP_STATUS_CREATED = 201,
	HTTP_STATUS_ACCEPTED = 202,
	HTTP_STATUS_NO_CONTENT = 204,
	HTTP_STATUS_MOVED_PERMANENTLY = 301,
	HTTP_STATUS_MOVED_TEMPORARILY = 302,
	HTTP_STATUS_NOT_MODIFIED = 304,
	HTTP_STATUS_BAD_REQUEST = 400,
	HTTP_STATUS_UNAUTHORIZED = 401,
	HTTP_STATUS_FORBIDDEN = 403,
	HTTP_STATUS_NOT_FOUND = 404,
	HTTP_STATUS_INTERNAL_SERVER_ERROR = 500,
	HTTP_STATUS_NOT_IMPLEMENTED = 501,
	HTTP_STATUS_BAD_GATEWAY = 502,
	HTTP_STATUS_SERVICE_UNAVAILABLE = 503,
};

/*
 * Validates the HTTP method.
 * Returns -1 on invalid method, 0 on success.
 */
int validate_method(const char *method);

/*
 * Converts a hexadecimal character to its integer value.
 * Returns -1 on invalid character.
 */
int hexval(int c);

/*
 * Normalizes the URI path by decoding percent-encoded characters.
 * Writes the normalized path to 'out' buffer of size 'outsz'.
 * Returns -1 on failure, 0 on success.
 */
int normalize_path(const char *uri_path, char *out, size_t outsz);

/*
 * Validates the URI.
 * Returns -1 on invalid URI, 0 on success.
 */
int validate_uri(const char *uri);

/*
 * Validates the HTTP version.
 * Returns -1 on invalid version, 0 on success.
 */
int validate_version(const char *version);

/*
 * Extracts the value of a specified header from a line.
 * Returns -1 if the header is not found, 0 on success.
 */
int extract_header(const char *line, const char *header_name,
                   char *header_value, size_t value_sz);

/*
 * Parses the request line into method, path, and version.
 * Returns -1 on failure, 0 on success.
 */
int parse_request_line(char *line, char *method, size_t method_sz, char *path,
                       size_t path_sz, char *version, size_t version_sz);

/*
 * Parses an HTTP request from the given stream into the http_request struct.
 * Returns an HTTP_PARSE_RESULT indicating success or type of failure.
 */
enum HTTP_PARSE_RESULT parse_http_request(FILE *stream,
                                          struct http_request *request);
/*
 * Crafts and writes an HTTP response to the given stream.
 * If is_head is non-zero, the body will not be included in the response.
 * Returns 0 on success.
 */
int craft_http_response(FILE *stream, enum HTTP_STATUS_CODE status_code,
                        const char *status_text, const char *body,
                        const char *content_type, const char *last_modified,
                        int is_head, struct http_response *resp);

/*
 * Handles a single HTTP connection on the given stream.
 * Uses server_config (docroot, cgi_dir, etc.) to route the request.
 * Returns 0 on success, -1 on error.
 */
int handle_http_connection(FILE *stream, const struct server_config *cfg,
                           struct http_request *req,
                           struct http_response *resp);
