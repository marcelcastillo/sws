#include "cgi.h"

#include <sys/wait.h>

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static int
cgi_build_script_path(const struct http_request *req, const char *cgi_dir,
                      char *script_path, size_t script_path_len,
                      char *script_name, size_t script_name_len,
                      const char **query_string_out)
{
	const char prefix[] = "/cgi-bin/";
	const char *uri = req->path;
	const char *path_start;
	const char *qmark;
	char script_rel[PATH_MAX];

	if (cgi_dir == NULL) {
		return -1;
	}

	/* URI must start with "/cgi-bin/" */
	if (strncmp(uri, prefix, sizeof(prefix) - 1) != 0) {
		return -1;
	}

	path_start = uri + (sizeof(prefix) - 1); /* after "/cgi-bin/" */
	qmark = strchr(path_start, '?');

	if (qmark != NULL) {
		size_t len = (size_t)(qmark - path_start);
		if (len >= sizeof(script_rel)) {
			return -1;
		}
		memcpy(script_rel, path_start, len);
		script_rel[len] = '\0';
		*query_string_out = qmark + 1;
	} else {
		strncpy(script_rel, path_start, sizeof(script_rel));
		script_rel[sizeof(script_rel) - 1] = '\0';
		*query_string_out = "";
	}

	/* SCRIPT_NAME should be /cgi-bin/<script_rel> without query string */
	if (snprintf(script_name, script_name_len, "/cgi-bin/%s", script_rel) >=
	    (int)script_name_len) {
		return -1;
	}

	/* Full filesystem path to the CGI script */
	if (snprintf(script_path, script_path_len, "%s/%s", cgi_dir, script_rel) >=
	    (int)script_path_len) {
		return -1;
	}

	return 0;
}

int
cgi_handle(FILE *stream, const struct http_request *req, const char *cgi_dir,
           int is_head, struct http_response *resp)
{
	char script_path[PATH_MAX];
	char script_name[PATH_MAX];
	const char *query_string;
	pid_t pid;
	int status;
	int pfd[2];

	if (cgi_build_script_path(req, cgi_dir, script_path, sizeof(script_path),
	                          script_name, sizeof(script_name),
	                          &query_string) < 0) {
		/* Not a CGI URI or bad mapping */
		return -1;
	}

	if (pipe(pfd) == -1) {
		return -1;
	}

	pid = fork();
	if (pid < 0) {
		close(pfd[0]);
		close(pfd[1]);
		return -1;
	}

	if (pid == 0) {
		/*
		 * Child: set up CGI environment and exec script.
		 * Now we write CGI output to the pipe, not directly to the socket.
		 */
		const char *method = req->method;

		/* Child doesn't read from the pipe */
		close(pfd[0]);

		if (dup2(pfd[1], STDOUT_FILENO) == -1) {
			_exit(1);
		}
		close(pfd[1]);

		if (setenv("REQUEST_METHOD", method, 1) == -1) {
			_exit(1);
		}
		if (setenv("QUERY_STRING", query_string, 1) == -1) {
			_exit(1);
		}
		if (setenv("SERVER_PROTOCOL", "HTTP/1.0", 1) == -1) {
			_exit(1);
		}
		if (setenv("SCRIPT_NAME", script_name, 1) == -1) {
			_exit(1);
		}
		if (setenv("GATEWAY_INTERFACE", "CGI/1.1", 1) == -1) {
			_exit(1);
		}
		/* REMOTE_ADDR etc. are optional; server.c may have already set them */

		execl(script_path, script_path, (char *)NULL);

		/* If we reach here, execl failed */
		_exit(127);
	}

	/* ---- Parent: read CGI output and wrap it in HTTP/1.0 ---- */

	close(pfd[1]); /* parent only reads */

	char tmp[4096];
	char *buf = NULL;
	size_t buf_len = 0, buf_cap = 0;
	ssize_t n;

	while ((n = read(pfd[0], tmp, sizeof(tmp))) > 0) {
		if (buf_len + (size_t)n > buf_cap) {
			size_t newcap = buf_cap ? buf_cap * 2 : 8192;
			while (newcap < buf_len + (size_t)n) {
				newcap *= 2;
			}
			char *nb = realloc(buf, newcap);
			if (!nb) {
				free(buf);
				close(pfd[0]);
				(void)waitpid(pid, &status, 0);
				return -1;
			}
			buf = nb;
			buf_cap = newcap;
		}
		memcpy(buf + buf_len, tmp, (size_t)n);
		buf_len += (size_t)n;
	}
	close(pfd[0]);

	/* Avoid zombies */
	(void)waitpid(pid, &status, 0);

	if (!buf || buf_len == 0) {
		free(buf);
		return -1;
	}

	/* Find header/body separator: try \r\n\r\n, then \n\n */
	size_t header_end = 0;
	for (size_t i = 0; i + 3 < buf_len; i++) {
		if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' &&
		    buf[i + 3] == '\n') {
			header_end = i + 4;
			break;
		}
	}
	if (header_end == 0) {
		for (size_t i = 0; i + 1 < buf_len; i++) {
			if (buf[i] == '\n' && buf[i + 1] == '\n') {
				header_end = i + 2;
				break;
			}
		}
	}

	const char *content_type = "text/plain";
	char ctype_buf[128];
	int have_ctype = 0;
	char *body = NULL;
	size_t body_len = 0;

	if (header_end > 0) {
		/* Parse CGI headers in [0, header_end) */
		size_t hdr_len = header_end;
		char *hdr = malloc(hdr_len + 1);
		if (!hdr) {
			free(buf);
			return -1;
		}
		memcpy(hdr, buf, hdr_len);
		hdr[hdr_len] = '\0';

		/* Normalize CRLF to LF */
		for (char *p = hdr; *p; p++) {
			if (*p == '\r') {
				*p = '\n';
			}
		}

		char *saveptr = NULL;
		char *line = strtok_r(hdr, "\n", &saveptr);
		while (line) {
			while (*line == ' ' || *line == '\t') {
				line++;
			}
			if (*line == '\0') {
				/* blank line => end of headers */
				break;
			}
			if (strncasecmp(line, "Content-Type:", 13) == 0) {
				char *val = line + 13;
				while (*val == ' ' || *val == '\t') {
					val++;
				}
				if (*val != '\0') {
					strncpy(ctype_buf, val, sizeof(ctype_buf) - 1);
					ctype_buf[sizeof(ctype_buf) - 1] = '\0';
					have_ctype = 1;
				}
			}
			line = strtok_r(NULL, "\n", &saveptr);
		}
		free(hdr);

		content_type = have_ctype ? ctype_buf : "text/plain";

		body_len = buf_len - header_end;
		body = malloc(body_len + 1);
		if (!body) {
			free(buf);
			return -1;
		}
		memcpy(body, buf + header_end, body_len);
		body[body_len] = '\0';
		free(buf);
	} else {
		/* No headers at all: whole output is body */
		body_len = buf_len;
		body = malloc(body_len + 1);
		if (!body) {
			free(buf);
			return -1;
		}
		memcpy(body, buf, body_len);
		body[body_len] = '\0';
		free(buf);
	}

	/* Wrap in a proper HTTP/1.0 response */
	craft_http_response(stream, HTTP_STATUS_OK, "OK", body, content_type, NULL,
	                    is_head, resp);

	free(body);
	return 0;
}
