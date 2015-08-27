#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/time.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>

#define DEFAULT_PORT	"7124"

#define CMD_START	1
#define CMD_ACK		2
#define CMD_STOP	3
#define CMD_DATA	4

struct nio_cmd {
	uint32_t cmd;
	uint32_t threads;
	uint32_t seq_lo;
	uint32_t seq_hi;
	uint32_t recv_lo;
	uint32_t recv_hi;
} __attribute__((packed));

enum states {
	STATE_START,
	STATE_START_SENT,
	STATE_STARTED,
	STATE_DYING,
};

struct thread_config {
	pthread_t thread;
	int running;
	int fd;
	int thread_num;
	uint64_t last_seq;
	uint64_t packets;
};

static struct thread_config *configs;

static int timeout;
static int polling;
static int port = 7124;
static volatile int should_stop;
static int threads = 1;
static int domain = AF_UNSPEC;
static const char *hostname = NULL;

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

	if (results == NULL) {
		fprintf(stderr, "Could not resolve host %s\n", hostname);
		return -1;
	}

	if (af == AF_UNSPEC) {
		af = AF_INET;
		for (rp = results; rp != NULL; rp = rp->ai_next)
			if (rp->ai_family == AF_INET6)
				af = AF_INET6;
	}

	for (rp = results; rp != NULL; rp = rp->ai_next) {
		if (rp->ai_family != af)
			continue;

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

	freeaddrinfo(results);

	return fd;
}

void *client_thread(void *data)
{
       struct thread_config *cfg = data;
       struct timeval tv;
       fd_set wfds;
       uint64_t i = 0;
       int fd;

       fd = cfg->fd;

       while (!should_stop) {
	       int ret;

	       if (!polling) {
		       tv.tv_sec  = 1;
		       tv.tv_usec = 0;

		       FD_ZERO(&wfds);
		       FD_SET(fd, &wfds);

		       ret = select(fd + 1, NULL, &wfds, NULL, &tv);

		       if (ret != 1)
			       continue;
	       }

	       if (polling || FD_ISSET(fd, &wfds)) {
		       uint64_t seq = (i * threads) + cfg->thread_num;
		       ssize_t sent;
		       int i;

		       for (i = 0; i < 16384; ++i) {
			       sent = send(fd, &seq, sizeof(seq), 0);
			       if (sent < 0)
				       break;
			       if (sent == sizeof(seq)) {
				       i += 1;
				       cfg->last_seq  = seq;
				       cfg->packets  += 1;
			       }
		       }
	       }
       }

       return NULL;
}

void *server_thread(void *data)
{
       struct thread_config *cfg = data;
       struct timeval tv;
       fd_set rfds;
       int fd;

       fd = cfg->fd;

       while (!should_stop) {
	       int ret;

	       if (!polling) {
		       tv.tv_sec  = 1;
		       tv.tv_usec = 0;

		       FD_ZERO(&rfds);
		       FD_SET(fd, &rfds);

		       ret = select(fd + 1, &rfds, NULL, NULL, &tv);
		       if (ret != 1)
			       continue;
	       }

	       if (polling || FD_ISSET(fd, &rfds)) {
		       uint64_t seq;
		       ssize_t bytes;
		       int i;

		       for (i = 0; i < 16384; ++i) {
			       bytes = recv(fd, &seq, sizeof(seq), 0);
			       if (bytes < 0)
				       break;
			       if (bytes == sizeof(seq)) {
				       cfg->last_seq  = seq;
				       cfg->packets  += 1;
			       }
		       }
	       }
       }

       return NULL;
}

void wait_for_threads(void)
{
	void *ret;
	int i;

	if (!configs)
		return;

	for (i = 0; i < threads; ++i) {
		if (configs[i].running)
			pthread_join(configs[i].thread, &ret);
		configs[i].running = 0;
		close(configs[i].fd);
	}

	threads = 0;
}

int create_threads(int server)
{
	int i;

	configs = malloc(sizeof(struct thread_config) * threads);
	if (configs == NULL) {
		perror("malloc");
		exit(EXIT_FAILURE);
	}

	memset(configs, 0, sizeof(struct thread_config) * threads);

	for (i = 0; i < threads; ++i) {
		char srv[16];
		int ret;

		snprintf(srv, 16, "%d", port + 1 + i);
		configs[i].fd = create_socket(domain, hostname, srv);
		if (configs[i].fd == -1) {
			printf("Failed to create socket\n");
			return -1;
		}

		configs[i].thread_num = i;

		if (server)
			ret = pthread_create(&configs[i].thread, NULL,
					server_thread, configs + i);
		else
			ret = pthread_create(&configs[i].thread, NULL,
					client_thread, configs + i);

		if (ret) {
			perror("pthread_create");
			return -1;
		}

		configs[i].running = 1;
	}

	return 0;
}

static uint64_t timediff(struct timeval *last, struct timeval *now)
{
       uint64_t val;

       val = (now->tv_sec - last->tv_sec - 1) * 1000000;
       val += (1000000 - last->tv_usec) + now->tv_usec;

       return val;
}

struct packet_stats {
	uint64_t packets;
	uint64_t seq;
};

static void fetch_stats(struct packet_stats *stats)
{
	int i;

	memset(stats, 0, sizeof(*stats));

	if (configs == NULL)
		return;

	for (i = 0; i < threads; ++i) {
		if (!configs[i].running)
			continue;

		stats->packets += configs[i].packets;
		if (stats->seq < configs[i].last_seq)
			stats->seq = configs[i].last_seq;
	}

	return;
}

static void get_server_stats(struct nio_cmd *cmd)
{
       struct packet_stats stats;

       memset(cmd, 0, sizeof(*cmd));

       fetch_stats(&stats);

       cmd->cmd     = htonl(CMD_DATA);
       cmd->seq_lo  = htonl(stats.seq & 0xffffffffULL);
       cmd->seq_hi  = htonl(stats.seq >> 32);
       cmd->recv_lo = htonl(stats.packets & 0xffffffffULL);
       cmd->recv_hi = htonl(stats.packets >> 32);
}

void ctrl_server(int fd)
{
	enum states state = STATE_START;
	struct sockaddr_storage remote;
	struct nio_cmd cmd, recv_cmd;
	fd_set rfds, wfds;
	int cmd_write = 0;
	socklen_t r_len;
	struct timeval tv, last, now;

	while (state != STATE_DYING) {
		int ret;

		gettimeofday(&now, NULL);
		if (state == STATE_STARTED &&
		    timediff(&last, &now) >= 1000000)
			cmd_write = 1;

		tv.tv_sec  = 1;
		tv.tv_usec = 0;

		FD_ZERO(&rfds);
		FD_ZERO(&wfds);

		FD_SET(fd, &rfds);
		if (cmd_write)
			FD_SET(fd, &wfds);

		ret = select(fd + 1, &rfds, &wfds, NULL, &tv);
		if (ret == -1) {
			if (errno == EINTR)
				continue;

			perror("select");
			exit(EXIT_FAILURE);
		}

		if (should_stop)
			break;

		if (FD_ISSET(fd, &wfds)) {
			int sent;

			if (state == STATE_START) {
				sent = sendto(fd, &cmd, sizeof(cmd), 0,
					      (struct sockaddr *)&remote, r_len);
				if (sent != sizeof(cmd)) {
					perror("sendto");
					exit(EXIT_FAILURE);
				}
				cmd_write = 0;
				state = STATE_STARTED;

				gettimeofday(&last, NULL);
				create_threads(1);
				printf("Server started\n");
			} else if (state == STATE_STARTED) {
				get_server_stats(&cmd);
				sent = sendto(fd, &cmd, sizeof(cmd), 0,
					      (struct sockaddr *)&remote, r_len);
				if (sent != sizeof(cmd)) {
					perror("sendto");
					exit(EXIT_FAILURE);
				}
				gettimeofday(&last, NULL);
				cmd_write = 0;
			}
		}

		if (FD_ISSET(fd, &rfds)) {
			ssize_t bytes;

			r_len = sizeof(remote);
			bytes = recvfrom(fd, &recv_cmd, sizeof(recv_cmd), 0,
					 (struct sockaddr *)&remote, &r_len);
			if (bytes != sizeof(recv_cmd))
				continue;

			switch (ntohl(recv_cmd.cmd)) {
			case CMD_START:
				if (state != STATE_START)
					break;

				threads = ntohl(recv_cmd.threads);


				memset(&cmd, 0, sizeof(cmd));
				cmd.cmd   = htonl(CMD_ACK);
				cmd_write = 1;

				break;
			case CMD_STOP:
				should_stop = 1;
				wait_for_threads();
				state = STATE_DYING;
				break;
			default:
				break;
			}
		}
	}
}

void ctrl_client(int fd)
{
	uint64_t sent_packets, last_sent_packets = 0;
	uint64_t packets, last_packets = 0;
	enum states state = STATE_START;
	struct nio_cmd cmd, recv_cmd;
	struct timeval tv, last, now;
	struct packet_stats stats;
	fd_set rfds, wfds;
	int cmd_write = 0;
	int got_data = 0;

	memset(&cmd, 0, sizeof(cmd));
	cmd.cmd     = htonl(CMD_START);
	cmd.threads = htonl(threads);
	cmd_write = 1;

	gettimeofday(&last, NULL);
	while (state != STATE_DYING) {
		int ret;

		tv.tv_sec  = 1;
		tv.tv_usec = 0;

		FD_ZERO(&rfds);
		FD_ZERO(&wfds);

		FD_SET(fd, &rfds);
		if (cmd_write)
			FD_SET(fd, &wfds);

		if (should_stop) {
			memset(&cmd, 0, sizeof(cmd));
			cmd.cmd   = htonl(CMD_STOP);
			cmd_write = 1;
		}

		ret = select(fd + 1, &rfds, &wfds, NULL, &tv);
		if (ret == -1) {
			if (errno == EINTR)
				continue;

			perror("select");
			exit(EXIT_FAILURE);
		}

		if (FD_ISSET(fd, &wfds)) {
			ssize_t sent;

			sent = send(fd, &cmd, sizeof(cmd), 0);
			if (sent != sizeof(cmd)) {
				perror("send");
				exit(EXIT_FAILURE);
			}

			if (state == STATE_START)
				state = STATE_START_SENT;

			if (should_stop)
				state = STATE_DYING;

			cmd_write = 0;
		}

		if (FD_ISSET(fd, &rfds)) {
			ssize_t bytes;
			uint32_t cmd;

			bytes = recv(fd, &recv_cmd, sizeof(recv_cmd), 0);
			if (bytes != sizeof(recv_cmd)) {
				perror("recv");
				continue;
			}

			cmd = ntohl(recv_cmd.cmd);

			switch (cmd) {
			case CMD_ACK:
				if (state == STATE_START_SENT) {
					create_threads(0);
					printf("Client started\n");
					if (timeout > 0)
						alarm(timeout);
					state = STATE_STARTED;
				}
				break;
			case CMD_DATA:
				if (state != STATE_STARTED)
					break;

				gettimeofday(&now, NULL);
				packets = ntohl(recv_cmd.recv_hi);
				packets  = (packets << 32) | ntohl(recv_cmd.recv_lo);

				fetch_stats(&stats);
				sent_packets = stats.packets;

				if (got_data) {
					uint64_t p = packets - last_packets;
					uint64_t s = sent_packets - last_sent_packets;

					printf("PPS: %ju Sent: %ju\n",
						(p * 1000000) / timediff(&last, &now),
						(s * 1000000) / timediff(&last, &now));
				}

				last_sent_packets = sent_packets;
				last_packets      = packets;
				last              = now;
				got_data          = 1;

				break;
			default:
				break;
			}
		}
	}

	wait_for_threads();
}

void usage(const char *prg)
{
	printf("Usage: %s [-s] [-c server] [-p port] [-n threads] [-4] [-h]\n", prg);
	printf("    -s            Server Mode - Wait for incoming packets\n");
	printf("    -r server     Client Mode - Send packets to server\n");
	printf("    -p port       UDP port to bind to\n");
	printf("    -n threads    Number of thread to start for sending/receiving\n");
	printf("    -l            Polling mode - Do not use select() in worker threads\n");
	printf("    -t timeout    Timeout in seconds after client should stop and exit\n");
	printf("    -4            Force use of IPv4\n");
	printf("    -6            Force use of IPv6\n");
	printf("    -h            Print this help message and exit\n");
}

int main(int argc, char **argv)
{
	const char *service = DEFAULT_PORT;
	int port = atoi(DEFAULT_PORT);
	int server = 0;
	int ctrl_fd;
	int opt;

	/* Parse options */
	while (1) {
		opt = getopt(argc, argv, "sr:p:t:h46l");
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
		case 'n':
			threads = atoi(optarg);
			break;
		case 'd':
			timeout = atoi(optarg);
			break;
		case '4':
			domain = AF_INET;
			break;
		case '6':
			domain = AF_INET6;
			break;
		case 'l':
			polling = 1;
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

	if (server)
		ctrl_server(ctrl_fd);
	else
		ctrl_client(ctrl_fd);

	close(ctrl_fd);

	return 0;
}
