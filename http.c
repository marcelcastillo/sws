#include "http.h"

#include <sys/stat.h>

#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "cgi.h"
#include "server.h"

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

static const char *
guess_content_type(const char *path)
{
	const char *dot = strrchr(path, '.');
	if (!dot) {
		return "text/plain";
	}

	if (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0) {
		return "text/html";
	}
	if (strcmp(dot, ".txt") == 0) {
		return "text/plain";
	}
	if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0) {
		return "image/jpeg";
	}
	if (strcmp(dot, ".png") == 0) {
		return "image/png";
	}

	return "application/octet-stream";
}

static int
serve_static_file(FILE *stream, const struct http_request *req,
                  const struct server_config *cfg)
{
	char fullpath[PATH_MAX];
	struct stat st;
	int fd;
	char *buf;
	ssize_t nread;
	size_t total = 0;

	if (cfg->docroot == NULL) {
		const char *body = "500 Internal Server Error\n";
		craft_http_response(stream, HTTP_STATUS_INTERNAL_SERVER_ERROR,
		                    "Internal Server Error", body, "text/plain", 0);
		return -1;
	}

	/* Build full path: docroot + path */
	if (snprintf(fullpath, sizeof(fullpath), "%s%s", cfg->docroot, req->path) >=
	    (int)sizeof(fullpath)) {
		const char *body = "414 Request-URI Too Long\n";
		craft_http_response(stream, HTTP_STATUS_BAD_REQUEST, "Bad Request",
		                    body, "text/plain", 0);
		return -1;
	}

	if (stat(fullpath, &st) == -1) {
		const char *body = "404 Not Found\n";
		craft_http_response(stream, HTTP_STATUS_NOT_FOUND, "Not Found", body,
		                    "text/plain", 0);
		return -1;
	}

	/* If it's a directory, try index.html for now */
	if (S_ISDIR(st.st_mode)) {
		char indexpath[PATH_MAX];

		if (snprintf(indexpath, sizeof(indexpath), "%s%s%s", cfg->docroot,
		             req->path,
		             (req->path[strlen(req->path) - 1] == '/') ? "" : "/") >=
		    (int)sizeof(indexpath)) {
			const char *body = "400 Bad Request\n";
			craft_http_response(stream, HTTP_STATUS_BAD_REQUEST, "Bad Request",
			                    body, "text/plain", 0);
			return -1;
		}

		strncat(indexpath, "index.html",
		        sizeof(indexpath) - strlen(indexpath) - 1);

		if (stat(indexpath, &st) == -1 || !S_ISREG(st.st_mode)) {
			const char *body = "403 Forbidden\n";
			craft_http_response(stream, HTTP_STATUS_FORBIDDEN, "Forbidden",
			                    body, "text/plain", 0);
			return -1;
		}

		/* Use index.html instead */
		strncpy(fullpath, indexpath, sizeof(fullpath));
		fullpath[sizeof(fullpath) - 1] = '\0';
	}

	if (!S_ISREG(st.st_mode)) {
		const char *body = "403 Forbidden\n";
		craft_http_response(stream, HTTP_STATUS_FORBIDDEN, "Forbidden", body,
		                    "text/plain", 0);
		return -1;
	}

	fd = open(fullpath, O_RDONLY);
	if (fd == -1) {
		const char *body = "403 Forbidden\n";
		craft_http_response(stream, HTTP_STATUS_FORBIDDEN, "Forbidden", body,
		                    "text/plain", 0);
		return -1;
	}

	/* Very simple: read whole file into memory */
	buf = malloc(st.st_size + 1);
	if (!buf) {
		close(fd);
		const char *body = "500 Internal Server Error\n";
		craft_http_response(stream, HTTP_STATUS_INTERNAL_SERVER_ERROR,
		                    "Internal Server Error", body, "text/plain", 0);
		return -1;
	}

	while ((nread = read(fd, buf + total, st.st_size - total)) > 0) {
		total += (size_t)nread;
	}

	close(fd);
	buf[total] = '\0';

	const char *ctype = guess_content_type(fullpath);
	/* Note: craft_http_response uses strlen(body) for Content-Length,
	   so this only works correctly for text files; that's fine for a first
	   pass. */
	craft_http_response(stream, HTTP_STATUS_OK, "OK", buf, ctype, 0);

	free(buf);
	return 0;
}

int
handle_http_connection(FILE *stream, const struct server_config *cfg)
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

		craft_http_response(stream, status, text, body, "text/plain", 0);
		return -1;
	}

	is_head = (strcmp(req.method, "HEAD") == 0);

	/* CGI: /cgi-bin/... and cgi_dir configured */
	if (cfg && cfg->cgi_dir && strncmp(req.path, "/cgi-bin/", 9) == 0) {
		fflush(stream); /* flush any buffered input/output */
		int fd = fileno(stream);
		if (fd == -1 || cgi_handle(fd, &req, cfg->cgi_dir) < 0) {
			const char *body = "500 Internal Server Error\n";
			craft_http_response(stream, HTTP_STATUS_INTERNAL_SERVER_ERROR,
			                    "Internal Server Error", body, "text/plain",
			                    is_head);
			return -1;
		}
		return 0;
	}

	/* HEAD: we can still reuse serve_static_file, then ignore body later if
	   needed. For now, we just serve normally; supporting HEAD fully is a later
	   polish. */
	if (serve_static_file(stream, &req, cfg) < 0) {
		/* serve_static_file already sent an error */
		return -1;
	}

	return 0;
}
