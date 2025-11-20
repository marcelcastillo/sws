#ifndef SERVER_H
#define SERVER_H

#include <netinet/in.h>
#include <sys/socket.h>

struct server_config{

    char *cgi_dir;
    int debug_mode;

    struct sockaddr_storage bind_addr;
    socklen_t bind_addrlen;
    int have_bind_address;

    char *logfile;
    int port;
};

void runServer(struct server_config *cfg);

#endif
