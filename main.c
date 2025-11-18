#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>

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

int
main(int argc, char *argv[])
{
	char *cgi_dir = NULL;
	int debug_mode = 0;
	char *bind_address = NULL;
	char *log_file = NULL;
	int port = 8080;

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
			bind_address = optarg;
			break;
		case 'l':
			log_file = optarg;
			break;
		case 'p':
			// This needs to change since atoi does not handle or report errors
			// probably use strtol to catch invalid ports
			port = atoi(optarg);
			break;
		case 'h':
			usage();
			exit(0);
		default:
			usage();
			exit(1);
		}
	}

	printf("CGI Directory: %s\n", cgi_dir ? cgi_dir : "None");
	printf("Debug Mode: %s\n", debug_mode ? "Enabled" : "Disabled");
	printf("Bind Address: %s\n", bind_address ? bind_address : "All");
	printf("Log File: %s\n", log_file ? log_file : "None");
	printf("Port: %d\n", port);
	return 0;
}
