#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <string.h>

#define QOTD_PORT 17
#define LISTEN_BACKLOG 64
#define MAX_MESSAGE_SIZE 65535
#define QOTD_MESSAGE_FILE "/etc/qotd.txt"

static char qotd_message[MAX_MESSAGE_SIZE];

static int read_qotd_message() {
  int fd, ret;

  fd = open(QOTD_MESSAGE_FILE, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    fprintf(stderr, "Error opening QOTD file: ");
    switch (errno) {
    case EACCES:
      fprintf(stderr, "Permission denied\n");
      return -1;
    case EINTR:
      fprintf(stderr, "Interrupted\n");
      return -1;
    case ENOENT:
      fprintf(stderr, "File not found\n");
      return -1;
    case ETXTBSY:
      fprintf(stderr, "File busy??\n");
      return -1;
    default:
      fprintf(stderr, "Other error (%d)\n", errno);
      return -1;
    }
  }

  ret = read(fd, qotd_message, MAX_MESSAGE_SIZE);
  if (ret < 0) {
    fprintf(stderr, "Error reading QOTD file: %d\n", errno);
  } else if (ret < MAX_MESSAGE_SIZE) {
    qotd_message[ret] = '\0';
  }

  if (close(fd) < 0) {
    fprintf(stderr, "Error closing file (?): %d\n", errno);
    ret = -1;
  }

  return ret;
}

static int setup_server(int tcp) {
  int fd, ret;
  struct sockaddr_in bindaddr;
  struct in_addr inetaddr;

  fd = socket(AF_INET, tcp == 1 ? SOCK_STREAM : SOCK_DGRAM, 0);
  if (fd < 0) {
    fprintf(stderr, "Error opening %s socket: %d\n", tcp == 1 ? "TCP" : "UDP", errno);
    return -errno;
  }

  ret = 1;
  ret = setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &ret, sizeof(ret));
  if (ret < 0) {
    fprintf(stderr, "Error setting SO_REUSEPORT on %s socket: %d. Proceeding\n", tcp == 1 ? "TCP" : "UDP", errno);
  }

  bindaddr.sin_family = AF_INET;
  bindaddr.sin_port = htons(QOTD_PORT);
  inetaddr.s_addr = htonl(0x00000000); // 0.0.0.0
  bindaddr.sin_addr = inetaddr;
  ret = bind(fd, (struct sockaddr *) &bindaddr, sizeof(bindaddr));
  if (ret < 0) {
    fprintf(stderr, "Error binding %s socket: %d\n", tcp == 1 ? "TCP" : "UDP", errno);
    return -errno;
  }

  if (tcp == 0) {
    return fd;
  }

  ret = listen(fd, LISTEN_BACKLOG);
  if (ret < 0) {
    fprintf(stderr, "Error listening on %s socket: %d\n", tcp == 1 ? "TCP" : "UDP", errno);
    return -errno;
  }

  return fd;
}

static int handle_tcp(int fd) {
  int ret;

  ret = send(fd, qotd_message, strnlen(qotd_message, MAX_MESSAGE_SIZE), 0);
  if (ret < 0) {
    fprintf(stderr, "Error sending message to client: %d\n", errno);
    return -errno;
  }

  ret = close(fd);
  if (ret < 0) {
    fprintf(stderr, "Error closing client: %d\n", errno);
    return -errno;
  }

  return 0;
}

static int handle_udp(int fd, struct sockaddr_in *addr) {
  int ret;

  ret = sendto(fd, qotd_message, strnlen(qotd_message, MAX_MESSAGE_SIZE), 0, (struct sockaddr *)addr, sizeof(*addr));
  if (ret < 0) {
    fprintf(stderr, "Error sending UDP message: %d\n", errno);
    return -errno;
  }

  return 0;
}

static void log_connection(struct sockaddr_in *addr, int tcp) {
  in_addr_t ipaddr;
  in_port_t port;

  port = ntohs(addr->sin_port);
  ipaddr = ntohl(addr->sin_addr.s_addr);
  printf("%s Connection from %d.%d.%d.%d:%d\n", tcp == 1 ? "TCP" : "UDP", (ipaddr & 0xFF000000) >> 24, (ipaddr & 0x00FF0000) >> 16, (ipaddr & 0x0000FF00) >>8, ipaddr & 0x000000FF, port, ipaddr);
}

int main (int argc, char **argv) {
  int i, ret, epoll_fd, epoll_num, tcp_server, udp_server;
  struct sockaddr_in accept_addr;
  struct epoll_event events[2];
  socklen_t accept_addr_len = sizeof(accept_addr);

  qotd_message[0] = '\0';

  ret = setvbuf(stdout, NULL, _IOLBF, 0);
  if (ret < 0) {
    fprintf(stderr, "Failed to line-buffer stdout: %d\n", errno);
    return 1;
  }

  ret = setvbuf(stderr, NULL, _IOLBF, 0);
  if (ret < 0) {
    fprintf(stderr, "Failed to line-buffer stderr: %d\n", errno);
    return 1;
  }

  tcp_server = setup_server(1);
  if (tcp_server < 0) {
    return 1;
  }

  udp_server = setup_server(0);
  if (udp_server < 0) {
    return 1;
  }

  //0 = TCP, 1 = UDP
  epoll_fd = epoll_create(EPOLL_CLOEXEC);
  if (epoll_fd < 0) {
    fprintf(stderr, "Error creating epoll FD: %d\n", errno);
    return 1;
  }

  for (i = 0; i < 2; i++) {
    events[i].events = EPOLLIN;
    events[i].data.fd = i == 0 ? tcp_server : udp_server;
    ret = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, events[i].data.fd, &events[i]);
    if (ret < 0) {
      fprintf(stderr, "Error adding epoll fd: %d\n", errno);
      return 1;
    }
  }

  while (1) {
    epoll_num = epoll_wait(epoll_fd, events, 2, -1);
    if (epoll_num < 0) {
      if (errno == EINTR)
	continue;
      fprintf(stderr, "Error while polling server sockets: %d\n", errno);
      return 1;
    }

    read_qotd_message();

    for (i = 0; i < epoll_num; i++) {
      if (!(events[i].events & EPOLLIN)) {
	fprintf(stderr, "Exceptional condition from epoll: %x\n", events[i].events);
	continue;
      }

      if (events[i].data.fd == tcp_server) {
	ret = accept(tcp_server, (struct sockaddr *) &accept_addr, &accept_addr_len);
	if (ret < 0) {
	  fprintf(stderr, "Error accepting client from TCP: %d\n", errno);
	  continue;
	}
	if (accept_addr_len > sizeof(accept_addr)) {
	  fprintf(stderr, "Received sockaddr bigger than sizeof(struct sockaddr_in)!!! HUH ???\n");
	} else
	  log_connection(&accept_addr, 1);

	handle_tcp(ret);
      } else if (events[i].data.fd == udp_server) {
	ret = recvfrom(udp_server, NULL, 0, 0, (struct sockaddr *) &accept_addr, &accept_addr_len);
	if (ret < 0) {
	  fprintf(stderr, "Error recving from UDP: %d\n", errno);
	  continue;
	}
	if (accept_addr_len > sizeof(accept_addr)) {
	  fprintf(stderr, "Received sockaddr bigger than sizeof(struct sockaddr_in)!!! HUH ??? NO MESSAGE SENT\n");
	} else {
	  log_connection(&accept_addr, 0);
	  handle_udp(udp_server, &accept_addr);
	}
      }
    }
  }
}
