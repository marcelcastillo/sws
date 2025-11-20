#pragma once

#include <stdio.h>

#define MAX_METHOD 16
#define MAX_URI 1024
#define MAX_VERSION 16
#define MAX_HEADER_VALUE 256

struct http_request {
	char method[MAX_METHOD];
	char path[MAX_URI];
	char version[MAX_VERSION];
	char if_modified_since[MAX_HEADER_VALUE];
};

/*
 * Validates the HTTP method.
 * Returns -1 on invalid method, 0 on success.
 */
int validate_method(const char *method);

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
 * Returns -1 on failure, 0 on success.
 */
int parse_http_request(FILE *stream, struct http_request *request);
