#include "cgi.h"

#include <sys/wait.h>

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
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
cgi_handle(int client_fd, const struct http_request *req, const char *cgi_dir)
{
	char script_path[PATH_MAX];
	char script_name[PATH_MAX];
	const char *query_string;
	pid_t pid;
	int status;

	if (cgi_build_script_path(req, cgi_dir, script_path, sizeof(script_path),
	                          script_name, sizeof(script_name),
	                          &query_string) < 0) {
		/* Not a CGI URI or bad mapping */
		return -1;
	}

	pid = fork();
	if (pid < 0) {
		/* fork failed */
		return -1;
	}

	if (pid == 0) {
		/*
		 * Child: set up CGI environment and exec script.
		 * For now, we let the CGI produce the full HTTP response
		 * (status line + headers + body).
		 */
		const char *method = req->method;

		if (setenv("REQUEST_METHOD", method, 1) == -1) {
			_exit(1);
		}
		if (setenv("QUERY_STRING", query_string, 1) == -1) {
			_exit(1);
		}
		if (setenv("SERVER_PROTOCOL", "HTTP/1.0", 1) == -1) {
			_exit(1);
		}

		/* New CGI-ish variables */
		if (setenv("SCRIPT_NAME", script_name, 1) == -1) {
			_exit(1);
		}
		if (setenv("GATEWAY_INTERFACE", "CGI/1.1", 1) == -1) {
			_exit(1);
		}

		/* REMOTE_ADDR will be set in server.c per connection */

		if (dup2(client_fd, STDOUT_FILENO) == -1) {
			_exit(1);
		}

		execl(script_path, script_path, (char *)NULL);

		/* If we reach here, execl failed */
		_exit(127);
	}

	/* Parent: wait for child (avoid zombies) */
	if (waitpid(pid, &status, 0) < 0) {
		return -1;
	}

	return 0;
}
