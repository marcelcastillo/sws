#include "server.h"

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <netinet/in.h>

#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "http.h"


#define BACKLOG 5

#define SLEEP 5

void
logRequest(struct server_config *config, const char *clientIP,
           struct http_request *req, struct http_response *resp)
{
	time_t now = time(NULL);
	struct tm gmt;
	gmtime_r(&now, &gmt);

	char timebuf[64];
	strftime(timebuf, sizeof(timebuf), "%Y-%m-%dT%H:%M:%SZ", &gmt);

	char logbuf[4096];
	snprintf(logbuf, sizeof(logbuf), "%s %s \"%s %s %s\" %d %zu", clientIP,
	         timebuf, req->method, req->path, req->version, resp->status_code,
	         resp->content_len);

	if (config->debug_mode) {
		printf("%s\n", logbuf);
	} else {
		FILE *fp = config->logfp;
		if (fp) {
			fprintf(fp, "%s\n", logbuf);
			fflush(fp);
		}
	}
}

int
createSocket(struct server_config *config)
{
	int sock;
	socklen_t length;
	struct sockaddr_storage server;
	struct sockaddr_in *sin;
	struct sockaddr_in6 *sin6;

	memset(&server, 0, sizeof(server));

	/* A server address was provided */
	if (config->have_bind_address) {
		memcpy(&server, &config->bind_addr, sizeof(server));
		length = config->bind_addrlen;

		/* Cast server as necessary */
		if (server.ss_family == AF_INET) {
			sin = (struct sockaddr_in *)&server;
			sin->sin_port = config->port;
			length = sizeof(*sin);
		} else if (server.ss_family == AF_INET6) {
			sin6 = (struct sockaddr_in6 *)&server;
			sin6->sin6_port = config->port;
			length = sizeof(*sin6);
		} else {
			perror("Invalid address domain");
			exit(EXIT_FAILURE);
			/* NOTREACHED */
		}
		/* No server address was provided */
	} else {
		sin6 = (struct sockaddr_in6 *)&server;
		sin6->sin6_family = AF_INET6;
		sin6->sin6_addr = in6addr_any;
		sin6->sin6_port = config->port;
		length = sizeof(*sin6);
	}

	if ((sock = socket(server.ss_family, SOCK_STREAM, 0)) < 0) {
		perror("Opening Stream Socket");
		exit(EXIT_FAILURE);
		/* NOTREACHED */
	}
	if (bind(sock, (struct sockaddr *)&server, length) != 0) {
		perror("Binding stream socket");
		exit(EXIT_FAILURE);
		/* NOTREACHED */
	}

	length = sizeof(server);
	if (getsockname(sock, (struct sockaddr *)&server, &length) != 0) {
		perror("getting socket name");
		exit(EXIT_FAILURE);
		/* NOTREACHED */
	}
	if (server.ss_family == AF_INET) {
		(void)printf("Socket has port #%d\n", ntohs(sin->sin_port));
	} else if (server.ss_family == AF_INET6) {
		(void)printf("Socket has port #%d\n", ntohs(sin6->sin6_port));
	}

	if (listen(sock, BACKLOG) < 0) {
		perror("listening");
		exit(EXIT_FAILURE);
		/* NOTREACHED */
	}

	return sock;
}

void
handleConnection(int fd, struct sockaddr_storage client,
                 struct server_config *config)
{
	int res;
	const char *rip;
	char addrbuf[INET6_ADDRSTRLEN];
	struct http_request req;
	struct http_response resp;

	/* Convert client address to string */
	if (client.ss_family == AF_INET) {
		struct sockaddr_in *sin = (struct sockaddr_in *)&client;
		rip = inet_ntop(AF_INET, &sin->sin_addr, addrbuf, sizeof(addrbuf));
	} else if (client.ss_family == AF_INET6) {
		struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&client;
		rip = inet_ntop(AF_INET6, &sin6->sin6_addr, addrbuf, sizeof(addrbuf));
	} else {
		rip = "unknown";
	}

	if (config->debug_mode) {
		printf("Client connected from %s\n", rip);
	}

	if (setenv("REMOTE_ADDR", rip, 1) == -1) {
		/* ignore */
	}

	{
		char portbuf[16];
		snprintf(portbuf, sizeof(portbuf), "%d", ntohs(config->port));
		setenv("SERVER_PORT", portbuf, 1);
	}

	{
		char hostbuf[256];
		if (gethostname(hostbuf, sizeof(hostbuf)) == 0) {
			hostbuf[sizeof(hostbuf) - 1] = '\0';
			setenv("SERVER_NAME", hostbuf, 1);
		}
	}

	FILE *stream = fdopen(fd, "r+");
	if (stream == NULL) {
		perror("fdopen");
		close(fd);
		exit(EXIT_FAILURE);
	}

	if ((res = handle_http_connection(stream, config, &req, &resp)) < 0) {
		if (config->debug_mode) {
			printf("Bad request\n");
		}
	}


	logRequest(config, rip, &req, &resp);


	fclose(stream);
	exit(EXIT_SUCCESS);
}

void
handleSocket(int server_sock, struct server_config *config)
{
	int fd;
	pid_t pid;
	struct sockaddr_storage client;
	socklen_t length = sizeof(client);

	memset(&client, 0, length);

	if ((fd = accept(server_sock, (struct sockaddr *)&client, &length)) < 0) {
		perror("Accept");
		return;
	}

	/* Debug mode does not fork */
	if (config->debug_mode) {
		/* No fork() in debug mode */
		handleConnection(fd, client, config);
		/* NOTREACHED */
	}

	if ((pid = fork()) < 0) {
		perror("fork");
		close(fd);
		return;
	}

	/* Child process */
	if (pid == 0) {
		handleConnection(fd, client, config);
		/* NOTREACHED */
	}

	/* Parent Process */
	close(fd);
	return;
}

/* Signal handler that reaps children */
void
reap(int sig)
{
	(void)sig;
	while (waitpid(-1, NULL, WNOHANG) > 0)
		;
}

void
runServer(struct server_config *config)
{
	int server_sock;

	if (signal(SIGCHLD, reap) == SIG_ERR) {
		perror("Signal");
		exit(EXIT_FAILURE);
	}

	server_sock = createSocket(config);

	/* Logging */
	if (config->logfile && !config->debug_mode) {
		if ((config->logfp = fopen(config->logfile, "a")) == NULL) {
			perror("fopen log file");
			exit(EXIT_FAILURE);
		}
	} else if (config->debug_mode) {
		config->logfp = stdout;
	}

	/* In debug mode... */
	if (config->debug_mode) {
		printf("Server running in debug mode.\n");
		handleSocket(server_sock, config);
		printf("Debug mode exiting.\n");
		return;
	}

	/* In normal mode... Daemonize. Don't change root */
	if (!config->debug_mode) {
		if (daemon(1, 0) < 0) {
			perror("daemon");
			exit(EXIT_FAILURE);
		}
	}

	for (;;) {
		fd_set ready;
		struct timeval timeout;

		FD_ZERO(&ready);
		FD_SET(server_sock, &ready);

		timeout.tv_sec = SLEEP;
		timeout.tv_usec = 0;

		if (select(server_sock + 1, &ready, 0, 0, &timeout) < 0) {
			if (errno != EINTR) {
				perror("select");
			}
			continue;
		}

		if (FD_ISSET(server_sock, &ready)) {
			handleSocket(server_sock, config);
		} else {
			(void)printf("Idly sitting here, waiting for connections...\n");
		}
	}
	return;
}
