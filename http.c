#include "http.h"

#include <sys/stat.h>

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <regex.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "cgi.h"
#include "server.h"

static time_t
parse_http_date(const char *s)
{
	struct tm tm;
	char *end;

	if (s == NULL || *s == '\0') {
		return (time_t)-1;
	}

	memset(&tm, 0, sizeof(tm));

	/* RFC 1123 / HTTP-date format: "Wed, 21 Oct 2015 07:28:00 GMT" */
	end = strptime(s, "%a, %d %b %Y %H:%M:%S GMT", &tm);
	if (end == NULL) {
		return (time_t)-1;
	}

	/* Interpret as GMT */
	return timegm(&tm);
}

int
validate_method(const char *method)
{
	if ((strcmp(method, "GET") == 0) || (strcmp(method, "HEAD") == 0)) {
		return 0;
	}
	return -1;
}

int
hexval(int c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	return -1;
}

int
normalize_path(const char *uri_path, char *out, size_t outsz)
{
	char tmp[4096];
	size_t ti = 0;

	// Percent-decode into tmp
	for (size_t i = 0; uri_path[i] != '\0';) {
		unsigned char c = (unsigned char)uri_path[i];

		if (c == '%') {
			int h1, h2;
			if (!uri_path[i + 1] || !uri_path[i + 2]) {
				return -1; // incomplete escape
			}
			h1 = hexval((unsigned char)uri_path[i + 1]);
			h2 = hexval((unsigned char)uri_path[i + 2]);
			if (h1 < 0 || h2 < 0) {
				return -1; // invalid hex
			}
			c = (unsigned char)((h1 << 4) | h2);
			i += 3;
		} else {
			i++;
		}

		if (ti + 1 >= sizeof(tmp)) {
			return -1; // overflow
		}
		tmp[ti++] = (char)c;
	}
	tmp[ti] = '\0';

	// Remove dot segments
	size_t out_len = 0;

	// Always work with a leading '/', since sws paths are rooted
	const char *p = tmp;
	if (*p != '/') {
		// treat relative like rooted at '/'
		if (out_len + 1 >= outsz) {
			return -1;
		}
		out[out_len++] = '/';
	} else {
		// copy initial slash
		if (out_len + 1 >= outsz) {
			return -1;
		}
		out[out_len++] = *p++;
	}

	while (*p != '\0') {
		// Skip repeated slashes
		if (*p == '/') {
			p++;
			continue;
		}

		// Find next segment [p, q)
		const char *seg_start = p;
		while (*p != '\0' && *p != '/') {
			p++;
		}
		const char *seg_end = p;
		size_t seg_len = (size_t)(seg_end - seg_start);

		// Segment content decisions: ".", "..", or normal
		if (seg_len == 1 && seg_start[0] == '.') {
			// "." -> skip
			continue;
		}
		if (seg_len == 2 && seg_start[0] == '.' && seg_start[1] == '.') {
			// ".." -> pop last segment, but not leading '/'
			// Remove trailing slash if present
			if (out_len > 1) {
				// drop trailing slash if any
				if (out[out_len - 1] == '/') {
					out_len--;
				}

				// walk back to previous '/'
				while (out_len > 1 && out[out_len - 1] != '/') {
					out_len--;
				}

				// if we ended up at '/', we effectively popped a segment
			} else {
				// would escape above root
				return -1;
			}
			continue;
		}

		// Normal segment: append "/" if needed, then segment
		if (out_len == 0 || out[out_len - 1] != '/') {
			if (out_len + 1 >= outsz) {
				return -1;
			}
			out[out_len++] = '/';
		}

		if (out_len + seg_len >= outsz) {
			return -1;
		}
		memcpy(out + out_len, seg_start, seg_len);
		out_len += seg_len;
	}

	// Special case: if result is empty, use "/"
	if (out_len == 0) {
		if (outsz < 2) {
			return -1;
		}
		out[0] = '/';
		out[1] = '\0';
	} else {
		out[out_len] = '\0';
	}

	return 0;
}

int
validate_uri(const char *uri)
{
	size_t len;
	const char *p;

	if (uri == NULL) {
		return -1;
	}

	/* Must be non-empty and start with '/' */
	if (uri[0] == '\0' || uri[0] != '/') {
		return -1;
	}

	len = strlen(uri);

	/* Optional: length check */
	if (len >= MAX_URI) {
		return -1;
	}

	/*
	 * Prevent directory traversal: reject URIs that contain
	 * "/../" or end with "/.." or start with "../".
	 */
	p = uri;
	while ((p = strstr(p, "..")) != NULL) {
		int at_start = (p == uri);
		int preceded_by_slash = (p > uri && p[-1] == '/');
		int followed_by_slash = (p[2] == '/');
		int ends_here = (p[2] == '\0');

		if ((at_start && (followed_by_slash || ends_here)) ||
		    (preceded_by_slash && (followed_by_slash || ends_here))) {
			return -1;
		}
		p += 2;
	}

	/* For this project we don't need to micro-validate every char.
	 * If it starts with '/', is not too long, and doesn't try to
	 * traverse "..", we accept it.
	 */
	return 0;
}

int
validate_version(const char *version)
{
	if (strcmp(version, "HTTP/1.0") == 0 || strcmp(version, "HTTP/1.1") == 0) {
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

	/*
	> >         char *m = strtok(p, " \t\r\n");
	>
	> I don't think that's right.  Your request line can't
	> contain either \r nor \n anywhere except at the end of
	> the line (and in fact MUST have both there); I don't
	> know whether the RFC allows tabs as a separator
	> between method, uri, and version, so check that as
	> well.
	*/

	// char *m = strtok(p, " \t\r\n");
	// char *u = strtok(NULL, " \t\r\n");
	// char *v = strtok(NULL, " \t\r\n");
	// if (!m || !u || !v) {
	// 	return -1;
	// }
	// strncpy(method, m, method_sz - 1);
	// method[method_sz - 1] = '\0';
	// strncpy(path, u, path_sz - 1);
	// path[path_sz - 1] = '\0';
	// strncpy(version, v, version_sz - 1);
	// version[version_sz - 1] = '\0';
	// return 0;

	// request line cant contain \r or \n except at end (which must have both)
	char *sp1 = strchr(p, ' ');
	if (!sp1) {
		return -1;
	}
	*sp1 = '\0';
	strncpy(method, p, method_sz - 1);
	method[method_sz - 1] = '\0';
	p = sp1 + 1;
	char *sp2 = strchr(p, ' ');
	if (!sp2) {
		return -1;
	}
	*sp2 = '\0';
	strncpy(path, p, path_sz - 1);
	path[path_sz - 1] = '\0';
	p = sp2 + 1;
	char *crlf = strstr(p, "\r\n");
	if (!crlf) {
		return -1;
	}
	*crlf = '\0';
	strncpy(version, p, version_sz - 1);
	version[version_sz - 1] = '\0';
	return 0;
}

enum HTTP_PARSE_RESULT
parse_http_request(FILE *stream, struct http_request *request)
{
	/*
	> >         char line[2048];
	>
	> You need to justify the line length.  The RFC may have
	> a limit for that.
	*/
	char line[2048];
	memset(request, 0, sizeof(*request));

	if (fgets(line, sizeof(line), stream) == NULL) {
		return HTTP_PARSE_EOF;
	}

	/* Store the original request in the http_request struct for logging */
	strncpy(request->request_line, line, sizeof(request->request_line) - 1);
	request->request_line[sizeof(request->request_line) - 1] = '\0';

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
                    const char *content_type, const char *last_modified,
                    int is_head, struct http_response *resp)
{
	time_t now = time(NULL);
	struct tm gmt;
	gmtime_r(&now, &gmt);
	char date_buf[64];
	strftime(date_buf, sizeof(date_buf), "%a, %d %b %Y %H:%M:%S GMT", &gmt);
	size_t len = body ? strlen(body) : 0;

	fprintf(stream, "HTTP/1.0 %d %s\r\n", status_code, status_text);
	fprintf(stream, "Date: %s\r\n", date_buf);
	fprintf(stream, "Server: sws/1.0\r\n");
	if (last_modified) {
		fprintf(stream, "Last-Modified: %s\r\n", last_modified);
	}
	fprintf(stream, "Content-Length: %zu\r\n", len);
	fprintf(stream, "Content-Type: %s\r\n",
	        content_type ? content_type : "text/plain");
	fprintf(stream, "\r\n");
	if (!is_head && body) {
		fprintf(stream, "%s", body);
	}

	if (resp) {
		resp->status_code = status_code;
		resp->content_len = len;
	}

	return 0;
}

static const char *
guess_content_type(const char *path)
{
	/*
	    This has to change to use magic(5)
	*/
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

struct dir_entry {
	char *name;
	int is_dir;
};

static int
dir_entry_cmp(const void *a, const void *b)
{
	const struct dir_entry *ea = a;
	const struct dir_entry *eb = b;
	return strcmp(ea->name, eb->name);
}

static int
serve_static_file(FILE *stream, const struct http_request *req,
                  const struct server_config *cfg, int is_head,
                  struct http_response *resp)
{
	char fullpath[PATH_MAX];
	struct stat st;
	int fd;
	char *buf;
	ssize_t nread;
	size_t total = 0;

	const char *uri = req->path;
	const char *base = NULL;    /* docroot / user-sws */
	const char *subpath = NULL; /* part after docroot */

	char user_root[PATH_MAX];

	/* ----- Decide base directory (docroot vs /~user) ----- */

	if (strncmp(uri, "/~", 2) == 0) {
		/* /~user[/... ] → /home/user/sws[/...] */
		const char *user_start = uri + 2;
		const char *slash = strchr(user_start, '/');
		char username[64];

		if (slash) {
			size_t ulen = (size_t)(slash - user_start);
			if (ulen == 0 || ulen >= sizeof(username)) {
				const char *body = "404 Not Found\n";
				craft_http_response(stream, HTTP_STATUS_NOT_FOUND, "Not Found",
				                    body, "text/plain", NULL, is_head, resp);
				return -1;
			}
			memcpy(username, user_start, ulen);
			username[ulen] = '\0';
			subpath = slash; /* includes leading '/' */
		} else {
			/* "/~user" with no trailing slash → treat as "/~user/" */
			size_t ulen = strlen(user_start);
			if (ulen == 0 || ulen >= sizeof(username)) {
				const char *body = "404 Not Found\n";
				craft_http_response(stream, HTTP_STATUS_NOT_FOUND, "Not Found",
				                    body, "text/plain", NULL, is_head, resp);
				return -1;
			}
			memcpy(username, user_start, ulen + 1);
			subpath = "/"; /* inside ~/sws */
		}

		struct passwd *pw = getpwnam(username);
		if (!pw) {
			const char *body = "404 Not Found\n";
			craft_http_response(stream, HTTP_STATUS_NOT_FOUND, "Not Found",
			                    body, "text/plain", NULL, is_head, resp);
			return -1;
		}

		if (snprintf(user_root, sizeof(user_root), "%s/sws", pw->pw_dir) >=
		    (int)sizeof(user_root)) {
			const char *body = "500 Internal Server Error\n";
			craft_http_response(stream, HTTP_STATUS_INTERNAL_SERVER_ERROR,
			                    "Internal Server Error", body, "text/plain",
			                    NULL, is_head, resp);
			return -1;
		}
		base = user_root;
	} else {
		base = cfg->docroot;
		subpath = uri;
	}

	if (base == NULL) {
		const char *body = "500 Internal Server Error\n";
		craft_http_response(stream, HTTP_STATUS_INTERNAL_SERVER_ERROR,
		                    "Internal Server Error", body, "text/plain", NULL,
		                    is_head, resp);
		return -1;
	}

	/* Build full path: base + subpath */
	if (snprintf(fullpath, sizeof(fullpath), "%s%s", base, subpath) >=
	    (int)sizeof(fullpath)) {
		const char *body = "414 Request-URI Too Long\n";
		craft_http_response(stream, HTTP_STATUS_BAD_REQUEST, "Bad Request",
		                    body, "text/plain", NULL, is_head, resp);
		return -1;
	}

	if (stat(fullpath, &st) == -1) {
		const char *body = "404 Not Found\n";
		craft_http_response(stream, HTTP_STATUS_NOT_FOUND, "Not Found", body,
		                    "text/plain", NULL, is_head, resp);
		return -1;
	}

	time_t ims = (time_t)-1;
	if (req->if_modified_since[0] != '\0') {
		ims = parse_http_date(req->if_modified_since);
	}

	/* ----- Directory handling (index.html or auto index) ----- */

	if (S_ISDIR(st.st_mode)) {
		char indexpath[PATH_MAX];
		struct stat st_index;

		if (snprintf(indexpath, sizeof(indexpath), "%s%s%s", base, subpath,
		             (subpath[strlen(subpath) - 1] == '/') ? "" : "/") >=
		    (int)sizeof(indexpath)) {
			const char *body = "400 Bad Request\n";
			craft_http_response(stream, HTTP_STATUS_BAD_REQUEST, "Bad Request",
			                    body, "text/plain", NULL, is_head, resp);
			return -1;
		}

		strncat(indexpath, "index.html",
		        sizeof(indexpath) - strlen(indexpath) - 1);

		/* If index.html exists and is a regular file, serve that */
		if (stat(indexpath, &st_index) == 0 && S_ISREG(st_index.st_mode)) {
			strncpy(fullpath, indexpath, sizeof(fullpath));
			fullpath[sizeof(fullpath) - 1] = '\0';
			st = st_index; /* use index's st for Last-Modified */
		} else {
			/* No index.html: 304 based on directory mtime */
			if (ims != (time_t)-1 && st.st_mtime <= ims) {
				craft_http_response(stream, HTTP_STATUS_NOT_MODIFIED,
				                    "Not Modified", NULL, NULL, NULL, is_head,
				                    resp);
				return 0;
			}

			/* No index.html: generate a directory index */
			DIR *dir = opendir(fullpath);
			if (!dir) {
				const char *body = "403 Forbidden\n";
				craft_http_response(stream, HTTP_STATUS_FORBIDDEN, "Forbidden",
				                    body, "text/plain", NULL, is_head, resp);
				return -1;
			}

			struct dir_entry *entries = NULL;
			size_t nent = 0, cap = 0;
			struct dirent *de;

			while ((de = readdir(dir)) != NULL) {
				const char *name = de->d_name;
				if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
					continue;
				}
				if (name[0] == '.') { /* hidden files ignored */
					continue;
				}

				if (nent == cap) {
					size_t newcap = cap ? cap * 2 : 16;
					struct dir_entry *tmp =
						realloc(entries, newcap * sizeof(*entries));
					if (!tmp) {
						closedir(dir);
						free(entries);
						const char *body = "500 Internal Server Error\n";
						craft_http_response(stream,
						                    HTTP_STATUS_INTERNAL_SERVER_ERROR,
						                    "Internal Server Error", body,
						                    "text/plain", NULL, is_head, resp);
						return -1;
					}
					entries = tmp;
					cap = newcap;
				}

				entries[nent].name = strdup(name);
				if (!entries[nent].name) {
					closedir(dir);
					for (size_t i = 0; i < nent; i++) {
						free(entries[i].name);
					}
					free(entries);
					const char *body = "500 Internal Server Error\n";
					craft_http_response(stream,
					                    HTTP_STATUS_INTERNAL_SERVER_ERROR,
					                    "Internal Server Error", body,
					                    "text/plain", NULL, is_head, resp);
					return -1;
				}

				/* Determine if this entry is a directory */
				char pathbuf[PATH_MAX];
				entries[nent].is_dir = 0;
				if (snprintf(pathbuf, sizeof(pathbuf), "%s/%s", fullpath,
				             name) < (int)sizeof(pathbuf) &&
				    stat(pathbuf, &st_index) == 0 &&
				    S_ISDIR(st_index.st_mode)) {
					entries[nent].is_dir = 1;
				}

				nent++;
			}
			closedir(dir);

			qsort(entries, nent, sizeof(*entries), dir_entry_cmp);

			/* Build HTML body */
			size_t body_cap = 8192;
			char *body = malloc(body_cap);
			if (!body) {
				for (size_t i = 0; i < nent; i++) {
					free(entries[i].name);
				}
				free(entries);
				const char *msg = "500 Internal Server Error\n";
				craft_http_response(stream, HTTP_STATUS_INTERNAL_SERVER_ERROR,
				                    "Internal Server Error", msg, "text/plain",
				                    NULL, is_head, resp);
				return -1;
			}
			body[0] = '\0';

			snprintf(body, body_cap,
			         "<html><head><title>Index of %s</title></head><body>\n"
			         "<h1>Index of %s</h1>\n<ul>\n",
			         req->path, req->path);

			for (size_t i = 0; i < nent; i++) {
				char line[PATH_MAX + 64];
				const char *slash = entries[i].is_dir ? "/" : "";
				snprintf(line, sizeof(line),
				         "<li><a href=\"%s%s\">%s%s</a></li>\n",
				         entries[i].name, slash, entries[i].name, slash);
				if (strlen(body) + strlen(line) + 1 < body_cap) {
					strcat(body, line);
				}
			}

			strncat(body, "</ul>\n</body></html>\n",
			        body_cap - strlen(body) - 1);

			/* Last-Modified from directory's mtime */
			char lastmod[64];
			struct tm gmt;
			gmtime_r(&st.st_mtime, &gmt);
			strftime(lastmod, sizeof(lastmod), "%a, %d %b %Y %H:%M:%S GMT",
			         &gmt);

			craft_http_response(stream, HTTP_STATUS_OK, "OK", body, "text/html",
			                    lastmod, is_head, resp);

			for (size_t i = 0; i < nent; i++) {
				free(entries[i].name);
			}
			free(entries);
			free(body);
			return 0;
		}
	}

	/* ----- Regular file serving ----- */

	/* For regular files, possibly return 304 instead of body */
	if (ims != (time_t)-1 && st.st_mtime <= ims) {
		craft_http_response(stream, HTTP_STATUS_NOT_MODIFIED, "Not Modified",
		                    NULL, NULL, NULL, is_head, resp);
		return 0;
	}

	if (!S_ISREG(st.st_mode)) {
		const char *body = "403 Forbidden\n";
		craft_http_response(stream, HTTP_STATUS_FORBIDDEN, "Forbidden", body,
		                    "text/plain", NULL, is_head, resp);
		return -1;
	}

	fd = open(fullpath, O_RDONLY);
	if (fd == -1) {
		const char *body = "403 Forbidden\n";
		craft_http_response(stream, HTTP_STATUS_FORBIDDEN, "Forbidden", body,
		                    "text/plain", NULL, is_head, resp);
		return -1;
	}

	buf = malloc(st.st_size + 1);
	if (!buf) {
		close(fd);
		const char *body = "500 Internal Server Error\n";
		craft_http_response(stream, HTTP_STATUS_INTERNAL_SERVER_ERROR,
		                    "Internal Server Error", body, "text/plain", NULL,
		                    is_head, resp);
		return -1;
	}

	while ((nread = read(fd, buf + total, st.st_size - total)) > 0) {
		total += (size_t)nread;
	}
	close(fd);
	buf[total] = '\0';

	const char *ctype = guess_content_type(fullpath);

	/* Last-Modified for this file */
	char lastmod[64];
	struct tm gmt;
	gmtime_r(&st.st_mtime, &gmt);
	strftime(lastmod, sizeof(lastmod), "%a, %d %b %Y %H:%M:%S GMT", &gmt);

	craft_http_response(stream, HTTP_STATUS_OK, "OK", buf, ctype, lastmod,
	                    is_head, resp);

	free(buf);
	return 0;
}

int
handle_http_connection(FILE *stream, const struct server_config *cfg,
                       struct http_request *req, struct http_response *resp)
{
	enum HTTP_PARSE_RESULT res;
	int is_head = 0;

	memset(req, 0, sizeof(*req));
	memset(resp, 0, sizeof(*resp));

	res = parse_http_request(stream, req);

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

		craft_http_response(stream, status, text, body, "text/plain", NULL, 0,
		                    resp);
		return -1;
	}

	is_head = (strcmp(req->method, "HEAD") == 0);

	/* CGI: /cgi-bin/... and cgi_dir configured */
	char norm[PATH_MAX];
	if (normalize_path(req->path, norm, sizeof(norm)) < 0) {
		const char *body = "400 Bad Request\n";
		craft_http_response(stream, HTTP_STATUS_BAD_REQUEST, "Bad Request",
		                    body, "text/plain", NULL, is_head, resp);
		return -1;
	}

	/* Use normalized path from here on */
	strncpy(req->path, norm, sizeof(req->path));
	req->path[sizeof(req->path) - 1] = '\0';

	if (cfg && cfg->cgi_dir && strncmp(req->path, "/cgi-bin/", 9) == 0) {
		fflush(stream); /* flush any buffered input/output */
		int fd = fileno(stream);
		if (fd == -1 || cgi_handle(fd, req, cfg->cgi_dir) < 0) {
			const char *body = "500 Internal Server Error\n";
			craft_http_response(stream, HTTP_STATUS_INTERNAL_SERVER_ERROR,
			                    "Internal Server Error", body, "text/plain",
			                    NULL, is_head, resp);
			return -1;
		}
		return 0;
	}

	/* HEAD: we can still reuse serve_static_file, then ignore body later if
	   needed. */
	if (serve_static_file(stream, req, cfg, is_head, resp) < 0) {
		/* serve_static_file already sent an error */
		return -1;
	}

	return 0;
}
