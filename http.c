#include "http.h"

#include <ctype.h>
#include <string.h>
#include <time.h>

int
validate_method(const char *method)
{
	if ((strcmp(method, "GET") == 0) || (strcmp(method, "HEAD") == 0)) {
		return 0;
	}
	return -1;
}

int
validate_uri(const char *uri)
{
	if (uri[0] != '/') {
		return -1;
	}

	if (strstr(uri, "..") != NULL) {
		return -1;
	}

	return 0;
}

int
validate_version(const char *version)
{
	if (strcmp(version, "HTTP/1.0") == 0) {
		return 0;
	}
	return -1;
}

int
extract_header(const char *line, const char *header_name, char *out,
               size_t out_sz)
{
	size_t name_len = strlen(header_name);
	if (strncasecmp(line, header_name, name_len) == 0 &&
	    line[name_len] == ':') {
		const char *val = line + name_len + 1;
		while (*val && isspace((unsigned char)*val)) {
			val++;
		}
		strncpy(out, val, out_sz - 1);
		out[out_sz - 1] = '\0';
		return 0;
	}
	return -1;
}

int
parse_request_line(char *line, char *method, size_t method_sz, char *path,
                   size_t path_sz, char *version, size_t version_sz)
{
	char *p = line;

	char *m = strtok(p, " \t\r\n");
	char *u = strtok(NULL, " \t\r\n");
	char *v = strtok(NULL, " \t\r\n");
	if (!m || !u || !v) {
		return -1;
	}
	strncpy(method, m, method_sz - 1);
	method[method_sz - 1] = '\0';
	strncpy(path, u, path_sz - 1);
	path[path_sz - 1] = '\0';
	strncpy(version, v, version_sz - 1);
	version[version_sz - 1] = '\0';
	return 0;
}

enum HTTP_PARSE_RESULT
parse_http_request(FILE *stream, struct http_request *request)
{
	char line[2048];
	memset(request, 0, sizeof(*request));

	if (fgets(line, sizeof(line), stream) == NULL) {
		return HTTP_PARSE_EOF;
	}

	if (parse_request_line(line, request->method, MAX_METHOD, request->path,
	                       MAX_URI, request->version, MAX_VERSION) != 0) {
		return HTTP_PARSE_LINE_FAILURE;
	}

	if (validate_method(request->method) == -1) {
		return HTTP_PARSE_INVALID_METHOD;
	}
	if (validate_uri(request->path) == -1) {
		return HTTP_PARSE_INVALID_URI;
	}
	if (validate_version(request->version) == -1) {
		return HTTP_PARSE_INVALID_VERSION;
	}

	request->if_modified_since[0] = '\0';
	while (fgets(line, sizeof(line), stream) != NULL) {
		if (extract_header(line, "If-Modified-Since",
		                   request->if_modified_since,
		                   sizeof(request->if_modified_since)) == 0) {
			continue;
		}

		if (strcmp(line, "\r\n") == 0 || strcmp(line, "\n") == 0) {
			break;
		}
	}

	return HTTP_PARSE_OK;
}

int
craft_http_response(FILE *stream, enum HTTP_STATUS_CODE status_code,
                    const char *status_text, const char *body,
                    const char *content_type, int is_head)
{
	time_t now = time(NULL);
	struct tm gmt;
	gmtime_r(&now, &gmt);
	char date_buf[64];
	strftime(date_buf, sizeof(date_buf), "%a, %d %b %Y %H:%M:%S GMT", &gmt);

	fprintf(stream, "HTTP/1.0 %d %s\r\n", status_code, status_text);
	fprintf(stream, "Date: %s\r\n", date_buf);
	fprintf(stream, "Server: sws/1.0\r\n");
	fprintf(stream, "Content-Length: %zu\r\n", strlen(body));
	fprintf(stream, "Content-Type: %s\r\n",
	        content_type ? content_type : "text/plain");
	fprintf(stream, "\r\n");
	if (!is_head && body) {
		fprintf(stream, "%s", body);
	}
	return 0;
}

int
handle_http_connection(FILE *stream)
{
	struct http_request req;
	enum HTTP_PARSE_RESULT res;
	int is_head = 0;

	memset(&req, 0, sizeof(req));

	res = parse_http_request(stream, &req);

	if (res != HTTP_PARSE_OK) {
		enum HTTP_STATUS_CODE status;
		const char *text;
		const char *body;

		switch (res) {
		case HTTP_PARSE_INVALID_METHOD:
			status = HTTP_STATUS_NOT_IMPLEMENTED;
			text = "Not Implemented";
			body = "501 Not Implemented\n";
			break;

		case HTTP_PARSE_INVALID_VERSION:
			/* For now, treat bad/unsupported versions as 400. */
			status = HTTP_STATUS_BAD_REQUEST;
			text = "Bad Request";
			body = "400 Bad Request\n";
			break;

		case HTTP_PARSE_INVALID_URI:
		case HTTP_PARSE_LINE_FAILURE:
		case HTTP_PARSE_EOF:
		default:
			status = HTTP_STATUS_BAD_REQUEST;
			text = "Bad Request";
			body = "400 Bad Request\n";
			break;
		}

		craft_http_response(stream, status, text, body, "text/plain",
		                    0 /* is_head */);
		return -1;
	}

	/* If we got here, parse_http_request validated method/URI/version. */
	is_head = (strcmp(req.method, "HEAD") == 0);

	/* For the snapshot: just send a simple 200 OK. */
	craft_http_response(stream, HTTP_STATUS_OK, "OK", "OK\n", "text/plain",
	                    is_head);
	return 0;
}
