#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <stdio.h>
#include <fcntl.h>

#define DEFAULT_PORT	"7124"

static int should_stop;
static int threads = 1;

static void sig_handler(int signum)
{
	fprintf(stderr, "Signal %d received - shutting down\n", signum);
	should_stop = 1;
}

static void set_nonblocking(int fd)
{
	int prev;

	if ((prev = fcntl(fd, F_GETFL, 0)) != -1)
		fcntl(fd, F_SETFL, prev | O_NONBLOCK);
}

int create_socket(int af, const char *hostname, const char *service)
{
	struct addrinfo hints, *results, *rp;
	int err, fd = -1;

	memset(&hints, 0, sizeof(hints));

	hints.ai_family   = af;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_flags    = AI_PASSIVE | AI_NUMERICSERV;

	err = getaddrinfo(hostname, service, &hints, &results);
	if (err) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(err));
		return -1;
	}

	for (rp = results; rp != NULL; rp = rp->ai_next) {
		fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (fd == -1)
			continue;

		if (hostname == NULL) {
			if (bind(fd, rp->ai_addr, rp->ai_addrlen) == 0)
				break;
		} else {
			if (connect(fd, rp->ai_addr, rp->ai_addrlen) != -1)
				break;
		}

		close(fd);
		fd = -1;
	}

	if (fd >= 0)
		set_nonblocking(fd);

	if (rp == NULL) {
		fprintf(stderr, "Could not resolve host %s\n", hostname);
		return -1;
	}

	freeaddrinfo(results);

	return fd;
}

void usage(const char *prg)
{
	printf("Usage: %s [-s] [-c server] [-p port] [-t threads] [-4] [-h]\n", prg);
	printf("    -s            Server Mode - Wait for incoming packets\n");
	printf("    -c server     Client Mode - Send packets to server\n");
	printf("    -p port       UDP port to bind to\n");
	printf("    -t threads    Number of thread to start for sending/receiving\n");
	printf("    -4            Force use of IPv4\n");
	printf("    -6            Force use of IPv6\n");
	printf("    -h            Print this help message and exit\n");
}

int main(int argc, char **argv)
{
	const char *service = DEFAULT_PORT;
	int port = atoi(DEFAULT_PORT);
	const char *hostname = NULL;
	int domain = AF_UNSPEC;
	int server = 0;
	int ctrl_fd;
	int opt;

	/* Parse options */
	while (1) {
		opt = getopt(argc, argv, "sr:p:t:h46");
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
			service = optarg;
			port = atoi(service);
			break;
		case 't':
			threads = atoi(optarg);
			break;
		case '4':
			domain = AF_INET;
			break;
		case '6':
			domain = AF_INET6;
			break;
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

	/* Setup signals */
	signal(SIGTERM, sig_handler);
	signal(SIGINT,  sig_handler);
	signal(SIGALRM, sig_handler);
	signal(SIGHUP,  sig_handler);
	signal(SIGQUIT, sig_handler);

	ctrl_fd = create_socket(domain, hostname, service);
	if (ctrl_fd == -1)
		return EXIT_FAILURE;

	close(ctrl_fd);

	return 0;
}
