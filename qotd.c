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
#define MAX_QOTD_SIZE 65535
#define QOTD_MESSAGE_FILE "/etc/qotd.txt"
#define LISTEN_BACKLOG 64
#define SIMUL_EPOLL_EVENTS 2
#define READ_BUF_SIZE 65537

#define P_FOUR 4
#define P_ECHO 7
#define P_DISCARD 9
#define P_QOTD 17

struct sockdef {
  int fd;
  int flags;
  int protocol; //0b...ba: b = 1 == server, a = 1 == tcp
};

static int protocols[] = {P_FOUR, P_ECHO, P_DISCARD, P_QOTD};
static char qotd_message[MAX_QOTD_SIZE];
static char read_buffer[READ_BUF_SIZE];

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

  ret = read(fd, qotd_message, MAX_QOTD_SIZE);
  if (ret < 0) {
    fprintf(stderr, "Error reading QOTD file: %d\n", errno);
  } else if (ret < MAX_QOTD_SIZE) {
    qotd_message[ret] = '\0';
  }

  if (close(fd) < 0) {
    fprintf(stderr, "Error closing file (?): %d\n", errno);
    ret = -1;
  }

  return ret;
}

static int setup_server(int tcp, int port, int epoll_fd) {
  int fd, ret;
  struct sockaddr_in bindaddr;
  struct in_addr inetaddr;
  struct sockdef *sock_info;
  struct epoll_event event;

  fd = socket(AF_INET, tcp == 1 ? SOCK_STREAM : SOCK_DGRAM, 0);
  if (fd < 0) {
    fprintf(stderr, "Error opening %s port %d: %d\n", tcp == 1 ? "TCP" : "UDP", port, errno);
    return -errno;
  }

  ret = 1;
  ret = setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &ret, sizeof(ret));
  if (ret < 0) {
    fprintf(stderr, "Error setting SO_REUSEPORT on %s port %d: %d. Proceeding\n", tcp == 1 ? "TCP" : "UDP", port, errno);
  }

  bindaddr.sin_family = AF_INET;
  bindaddr.sin_port = htons(port);
  inetaddr.s_addr = htonl(0x00000000); // 0.0.0.0
  bindaddr.sin_addr = inetaddr;
  ret = bind(fd, (struct sockaddr *) &bindaddr, sizeof(bindaddr));
  if (ret < 0) {
    fprintf(stderr, "Error binding %s port %d: %d\n", tcp == 1 ? "TCP" : "UDP", port, errno);
    ret = -errno;
    goto err;
  }

  if (tcp == 1) {
    ret = listen(fd, LISTEN_BACKLOG);
    if (ret < 0) {
      fprintf(stderr, "Error listening on %s port %d: %d\n", tcp == 1 ? "TCP" : "UDP", port, errno);
      ret = -errno;
      goto err;
    }
  }

  sock_info = (struct sockdef *)malloc(sizeof(struct sockdef));
  if (sock_info == NULL) {
    fprintf(stderr, "Error allocating sockdef for %s port %d\n", tcp == 1 ? "TCP" : "UDP", port);
    ret = -errno;
    goto err;
  }
  sock_info->fd = fd;
  sock_info->protocol = port;
  sock_info->flags = 2 | tcp;

  event.events = EPOLLIN;
  event.data.ptr = sock_info;

  ret = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event);
  if (ret < 0) {
    fprintf(stderr, "Error adding %s port %d to epoll: %d\n", tcp == 1 ? "TCP" : "UDP", port, errno);
    ret = -errno;
    goto err;
  }

  return fd;

 err:
  close(fd);
  return ret;
}

static int handle_tcp(struct sockdef *sock_info) {
  int i, ret, pos;

  if (sock_info->protocol == P_QOTD) {
    read_qotd_message();
    ret = write(sock_info->fd, qotd_message, strnlen(qotd_message, MAX_QOTD_SIZE));
    if (ret < 0) {
      if (errno != ECONNRESET)
	fprintf(stderr, "Error sending message to client: %d\n", errno);
      ret = -errno;
    } else
      ret = -1; // To close it and remove from epoll
    return ret;
  }

  ret = read(sock_info->fd, read_buffer, READ_BUF_SIZE);
  if (ret < 0) {
    if (errno != ECONNRESET)
      fprintf(stderr, "Error reading from TCP port %d: %d\n", sock_info->protocol, errno);
    return -errno;
  } else if (ret == 0)
    return -1;

  if (sock_info->protocol == P_FOUR) {
    pos = 0;
    for (i = 0; i < ret; i++) {
      if (read_buffer[i] == '4')
	read_buffer[pos++] = '4';
    }
    if (pos == 0)
      return 0;
    ret = pos;
  }

  if (sock_info->protocol == P_DISCARD)
    return 0;

  ret = write(sock_info->fd, read_buffer, ret);
  if (ret < 0) {
    if (errno != ECONNRESET)
      fprintf(stderr, "Error sending message to TCP port %d: %d\n", sock_info->protocol, errno);
    return -errno;
  }
  return 0;
}

static int handle_udp(struct sockdef *sock_info, struct sockaddr_in *addr, char *buf, int buflen) {
  int i, ret;

  if (sock_info->protocol == P_DISCARD)
    return 0;
  else if (sock_info->protocol == P_QOTD) {
    read_qotd_message();
    buf = qotd_message;
    buflen = strnlen(buf, MAX_QOTD_SIZE);
  } else if (sock_info->protocol == P_FOUR) {
    ret = 0;
    for (i = 0; i < buflen; i++) {
      if (buf[i] == '4')
	buf[ret++] = '4';
    }
    buflen = ret;
  }

  ret = sendto(sock_info->fd, buf, buflen, 0, (struct sockaddr *)addr, sizeof(*addr));
  if (ret < 0) {
    fprintf(stderr, "Error sending UDP port %d message: %d\n", sock_info->protocol, errno);
    return -errno;
  }
  return 0;
}

static void log_connection(struct sockaddr_in *addr, struct sockdef *sock_info) {
  in_addr_t ipaddr;
  in_port_t port;
  char *proto_str;

  switch (sock_info->protocol) {
  case P_QOTD:
    proto_str = "QOTD";
    break;
  case P_DISCARD:
    proto_str = "DISC";
    break;
  case P_FOUR:
    proto_str = "FOUR";
    break;
  case P_ECHO:
    proto_str = "ECHO";
    break;
  }

  port = ntohs(addr->sin_port);
  ipaddr = ntohl(addr->sin_addr.s_addr);
  printf("%s %s Connection from %d.%d.%d.%d:%d\n",
	 (sock_info->flags & 1) ? "TCP" : "UDP",
	 proto_str,
	 (ipaddr & 0xFF000000) >> 24,
	 (ipaddr & 0x00FF0000) >> 16,
	 (ipaddr & 0x0000FF00) >> 8,
	 ipaddr & 0x000000FF,
	 port,
	 ipaddr);
}

static void handle(struct epoll_event *event, int epoll_fd) {
  int ret, tcp_port;
  struct sockaddr_in accept_addr;
  socklen_t accept_addr_len = sizeof(accept_addr);
  struct sockdef *curr_sock_info;

  if (!(event->events & EPOLLIN)) {
    fprintf(stderr, "Exceptional condition from epoll: %x\n", event->events);
    return;
  }

  curr_sock_info = (struct sockdef *)event->data.ptr;

  if (curr_sock_info->flags & 1) { // TCP
    if (curr_sock_info->flags & 2) { // Server
      ret = accept(curr_sock_info->fd, (struct sockaddr *) &accept_addr, &accept_addr_len);
      if (ret < 0) {
	fprintf(stderr, "Error accepting client from TCP: %d\n", errno);
	return;
      }
      if (accept_addr_len > sizeof(accept_addr))
	fprintf(stderr, "Received sockaddr bigger than sizeof(struct sockaddr_in)!!! HUH ???\n");
      else {
	log_connection(&accept_addr, curr_sock_info);
	tcp_port = curr_sock_info->protocol;
	curr_sock_info = (struct sockdef *)malloc(sizeof(struct sockdef));
	curr_sock_info->fd = ret;
	curr_sock_info->flags = 1;
	curr_sock_info->protocol = tcp_port;
	event->events = EPOLLIN;
	event->data.ptr = curr_sock_info;
	ret = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, curr_sock_info->fd, event);
	if (ret < 0) {
	  fprintf(stderr, "Error adding new TCP connection to epoll: %d\n", errno);
	  close(curr_sock_info->fd);
	}
	if (tcp_port != P_QOTD)
	  return;
      }
    }
    ret = handle_tcp(curr_sock_info);
    if (ret < 0) {
      ret = epoll_ctl(epoll_fd, EPOLL_CTL_DEL, curr_sock_info->fd, NULL);
      if (ret < 0) {
	fprintf(stderr, "Error removing fd %d from epoll: %d\n", curr_sock_info->fd, errno);
      }
      close(curr_sock_info->fd);
      free(curr_sock_info);
    }
  } else { // UDP
    ret = recvfrom(curr_sock_info->fd, read_buffer, READ_BUF_SIZE, 0, (struct sockaddr *) &accept_addr, &accept_addr_len);
    if (ret < 0) {
      fprintf(stderr, "Error recving from UDP: %d\n", errno);
      return;
    }
    if (accept_addr_len > sizeof(accept_addr))
      fprintf(stderr, "Received sockaddr bigger than sizeof(struct sockaddr_in)!!! HUH ??? NO MESSAGE SENT\n");
    else {
      log_connection(&accept_addr, curr_sock_info);
      handle_udp(curr_sock_info, &accept_addr, read_buffer, ret);
    }
  }
}

int main (int argc, char **argv) {
  int i, ret, epoll_fd;
  struct epoll_event events[SIMUL_EPOLL_EVENTS];

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

  qotd_message[0] = '\0';

  //0 = TCP, 1 = UDP
  epoll_fd = epoll_create(EPOLL_CLOEXEC);
  if (epoll_fd < 0) {
    fprintf(stderr, "Error creating epoll FD: %d\n", errno);
    return 1;
  }

  for (i = 0; i < sizeof(protocols) / sizeof(protocols[0]); i++) {
    ret = setup_server(1, protocols[i], epoll_fd);
    if (ret < 0) {
      return 1;
    }

    ret = setup_server(0, protocols[i], epoll_fd);
    if (ret < 0) {
      return 1;
    }
  }

  while (1) {
    ret = epoll_wait(epoll_fd, events, SIMUL_EPOLL_EVENTS, -1);
    if (ret < 0) {
      if (errno == EINTR)
	continue;
      fprintf(stderr, "Error while polling server sockets: %d\n", errno);
      return 1;
    }

    for (i = 0; i < ret; i++) {
      handle(&events[i], epoll_fd);
    }
  }
}
