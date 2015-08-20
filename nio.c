#include <netinet/in.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

static int threads = 1;

void usage(const char *prg)
{
	printf("Usage: %s [-s] [-c server] [-p port] [-t threads] [-4] [-h]\n", prg);
	printf("    -s            Server Mode - Wait for incoming packets\n");
	printf("    -c server     Client Mode - Send packets to server\n");
	printf("    -p port       UDP port to bind to\n");
	printf("    -t threads    Number of thread to start for sending/receiving\n");
	printf("    -4            Force use of IPv4 (default is IPv6)\n");
	printf("    -h            Print this help message and exit\n");
}

int main(int argc, char **argv)
{
	const char *hostname = NULL;
	int domain = AF_INET6;
	int server = 0;
	int port = -1;
	int opt;

	/* Parse options */
	while (1) {
		opt = getopt(argc, argv, "sr:p:t:h4");
		if (opt == EOF)
			break;

		switch (opt) {
		case 's':
			server = 1;
			break;
		case 'r':
			hostname = optarg;
			break;
		case 'p':
			port = atoi(optarg);
			break;
		case 't':
			threads = atoi(optarg);
			break;
		case '4':
			domain = AF_INET;
		default:
			fprintf(stderr, "ERROR: Unknown option: %c\n", opt);
			/* fall-through */
		case 'h':
			usage(argv[0]);
			exit(EXIT_SUCCESS);
		}
	}

	/* Some sanity checks */
	if (hostname && server) {
		fprintf(stderr, "Only one of -s or -r is allowed\n");
		usage(argv[0]);
		exit(EXIT_FAILURE);
	}

	if (server && port < 0) {
		fprintf(stderr, "Option -s requires also -p");
		usage(argv[0]);
		exit(EXIT_FAILURE);
	}

	if (threads < 1) {
		fprintf(stderr, "Invalid number of threads: %d\n", threads);
		usage(argv[0]);
		exit(EXIT_FAILURE);
	}

	return 0;
}
