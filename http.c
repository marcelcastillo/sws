#include "http.h"

#include <ctype.h>
#include <string.h>

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

int
parse_http_request(FILE *stream, struct http_request *request)
{
	char line[2048];
	memset(request, 0, sizeof(*request));

	if (fgets(line, sizeof(line), stream) == NULL) {
		return -1;
	}

	if (parse_request_line(line, request->method, MAX_METHOD, request->path,
	                       MAX_URI, request->version, MAX_VERSION) != 0) {
		return -1;
	}

	if (validate_method(request->method) == -1) {
		return -1;
	}
	if (validate_uri(request->path) == -1) {
		return -1;
	}
	if (validate_version(request->version) == -1) {
		return -1;
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

	return 0;
}
