#include <netinet/in.h>

#include <arpa/inet.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "server.h"

static void
usage()
{
	printf("Usage: sws [options]\n");
	printf("Options:\n");
	printf("  -c dir      Allow execution of CGIs from the given directory.\n");
	printf("  -d          Enter debugging mode.\n");
	printf("  -h          Print this usage summary and exit.\n");
	printf("  -i address  Bind to the given IPv4 or IPv6 address (default: "
	       "all).\n");
	printf("  -l file     Log all requests to the given file.\n");
	printf("  -p port     Listen on the given port (default: 8080).\n");
}

/*
 * Validate and convert port number from string to in_port_t.
 */
in_port_t
validate_port(const char *port_str)
{
	char *endptr;
	long port = strtol(port_str, &endptr, 10);

	if (*endptr != '\0' || port < 1 || port > 65535) {
		fprintf(stderr, "Invalid port number: %s\n", port_str);
		exit(1);
	}

	return htons((in_port_t)port);
}

/*
 * Validate and convert IPv4/IPv6 address from string to binary form.
 */
void
validate_address(const char *addr_str, struct sockaddr_storage *storage)
{
	struct in_addr ipv4;
	struct in6_addr ipv6;

	/* IPv4 */
	if (inet_pton(AF_INET, addr_str, &ipv4) == 1) {
		struct sockaddr_in *sin = (struct sockaddr_in *)storage;
		memset(sin, 0, sizeof(*sin));
		sin->sin_family = AF_INET;
		sin->sin_addr = ipv4;
		return;
	}

	/* IPv6 */
	if (inet_pton(AF_INET6, addr_str, &ipv6) == 1) {
		struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)storage;
		memset(sin6, 0, sizeof(*sin6));
		sin6->sin6_family = AF_INET6;
		sin6->sin6_addr = ipv6;
		return;
	}

	fprintf(stderr, "Invalid IP address: %s\n", addr_str);
	exit(1);
}

/*
 * Helper for printing the parsed command-line options.
 */
void
print_options(char *cgi_dir, int debug_mode, struct sockaddr_storage *bind_addr,
              int bind_addrlen, int have_bind_address, char *log_file,
              in_port_t port)
{
	printf("CGI Directory: %s\n", cgi_dir ? cgi_dir : "None");
	printf("Debug Mode: %s\n", debug_mode ? "Enabled" : "Disabled");
	if (have_bind_address) {
		char addr_str[INET6_ADDRSTRLEN];
		if (bind_addr->ss_family == AF_INET) {
			struct sockaddr_in *sin = (struct sockaddr_in *)bind_addr;
			inet_ntop(AF_INET, &sin->sin_addr, addr_str, sizeof(addr_str));
		} else if (bind_addr->ss_family == AF_INET6) {
			struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)bind_addr;
			inet_ntop(AF_INET6, &sin6->sin6_addr, addr_str, sizeof(addr_str));
		} else {
			strcpy(addr_str, "Unknown family");
		}
		printf("Bind Address: %s\n", addr_str);
	} else {
		printf("Bind Address: All\n");
	}
	printf("Bind Address Length: %d\n", bind_addrlen);
	printf("Log File: %s\n", log_file ? log_file : "None");
	printf("Port: %d\n", ntohs(port));
}

int
main(int argc, char *argv[])
{
	char *cgi_dir = NULL;
	int debug_mode = 0;
	struct sockaddr_storage bind_addr;
	socklen_t bind_addrlen = 0;
	int have_bind_address = 0;

	char *log_file = NULL;
	in_port_t port = htons(8080);
	struct server_config config;

	memset(&config, 0, sizeof(config));

	char *docroot = NULL;

	int option;


	while ((option = getopt(argc, argv, "c:di:l:p:h")) != -1) {
		switch (option) {
		case 'c':
			cgi_dir = optarg;
			break;
		case 'd':
			debug_mode = 1;
			break;
		case 'i':
			validate_address(optarg, &bind_addr);
			have_bind_address = 1;
			bind_addrlen = (bind_addr.ss_family == AF_INET)
			                   ? sizeof(struct sockaddr_in)
			                   : sizeof(struct sockaddr_in6);
			break;
		case 'l':
			log_file = optarg;
			break;
		case 'p':
			port = validate_port(optarg);
			break;
		case 'h':
			usage();
			exit(0);
		default:
			usage();
			exit(1);
		}
	}

	if (optind < argc) {
		docroot = argv[optind];
	} else {
		fprintf(stderr, "Missing required document root directory argument!\n");
		usage();
		exit(1);
	}


	print_options(cgi_dir, debug_mode, &bind_addr, bind_addrlen,
	              have_bind_address, log_file, port);

	/* Build the config struct */
	config.cgi_dir = cgi_dir;
	config.debug_mode = debug_mode;
	config.bind_addr = bind_addr;
	config.bind_addrlen = bind_addrlen;
	config.have_bind_address = have_bind_address;
	config.logfile = log_file;
	config.port = port;
	config.docroot = docroot;

	runServer(&config);

	return 0;
}

// #include <stdio.h>
// #include <stdlib.h>
// #include <string.h>
//
// #include "http.h"
//
// int
// main()
// {
// 	const char *test_request =
// 		"GET /index.html HTTP/1.1 \r\n"
// 		"Host: localhost\r\n"
// 		"If-Modified-Since: Wed, 21 Oct 2015 07:28:00 GMT\r\n"
// 		"\r\n";
//
// 	FILE *stream = tmpfile();
// 	if (!stream) {
// 		perror("tmpfile");
// 		return 1;
// 	}
// 	fwrite(test_request, 1, strlen(test_request), stream);
// 	rewind(stream);
//
// 	struct http_request req;
// 	if (parse_http_request(stream, &req) == 0) {
// 		printf("Method: %s\n", req.method);
// 		printf("URI: %s\n", req.path);
// 		printf("Version: %s\n", req.version);
// 		printf("If-Modified-Since: %s\n", req.if_modified_since);
// 	} else {
// 		printf("Failed to parse HTTP request\n");
// 	}
//
// 	fclose(stream);
// 	return 0;
// }
