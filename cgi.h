#pragma once

#include <stdio.h>

#include "http.h"

/*
 * Execute a CGI script for the given request and wrap its output
 * in a proper HTTP/1.0 response.
 *
 * stream   - stdio wrapper around client_fd
 * req      - parsed HTTP request
 * cgi_dir  - directory passed via -c where CGI binaries/scripts live
 * is_head  - non-zero if this was a HEAD request
 * resp     - filled with status code and content length
 *
 * Returns 0 on success, -1 on error.
 */
int cgi_handle(FILE *stream, const struct http_request *req,
               const char *cgi_dir, int is_head, struct http_response *resp);
