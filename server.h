#ifndef SERVER_H
#define SERVER_H

#include <sys/socket.h>
#include <netinet/in.h>

#include <stdio.h>

struct server_config
{

	char *cgi_dir;
	int debug_mode;

	struct sockaddr_storage bind_addr;
	socklen_t bind_addrlen;
	int have_bind_address;

	char *logfile;
	FILE *logfp;

	int port;

	char *docroot;
};

void runServer(struct server_config *cfg);

#endif
