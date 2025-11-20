#pragma once

#include "http.h"

/*
 * Execute a CGI script for the given request.
 *
 * Parameters:
 *   client_fd - connected socket to the client
 *   req       - parsed HTTP request (method, path, version, etc.)
 *   cgi_dir   - directory passed via -c where CGI binaries/scripts live
 *
 * Returns:
 *   0 on success,
 *  -1 on error (e.g., not a CGI URI, script not found, fork/exec failure).
 *
 * NOTE:
 *   This assumes that any URI beginning with "/cgi-bin/" should be
 *   handled as a CGI request, with the path after "/cgi-bin/" resolved
 *   relative to cgi_dir.
 */
int cgi_handle(int client_fd, const struct http_request *req,
               const char *cgi_dir);
