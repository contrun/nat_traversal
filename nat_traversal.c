#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "nat_traversal.h"
#include "utils.h"

#define MAX_PORT 65535
#define MIN_PORT 1025
#define NUM_OF_PORTS 700

#define MSG_BUF_SIZE 512

// file scope variables
static int ports[MAX_PORT - MIN_PORT];

static int send_to_punch_server(client *c) {
  verbose_log("sending %ld bytes of data to punch server\n",
              c->msg_buf - c->buf);
  hex_dump(NULL, c->buf, c->msg_buf - c->buf);
  int n = send(c->sfd, c->buf, c->msg_buf - c->buf, 0);
  c->msg_buf = c->buf;
  if (n == -1) {
    verbose_log("send to punch server, error number: %d, error: %s\n", errno,
                strerror(errno));
    return n;
  }
  return 0;
}

int get_peer_info(client *cli, uint32_t peer_id, struct peer_info *peer) {
  cli->msg_buf = cli->buf;
  cli->msg_buf = encode16(cli->msg_buf, GetPeerInfo);
  cli->msg_buf = encode32(cli->msg_buf, peer_id);
  return send_get_peer_info_request(cli, peer);
}

int get_peer_info_from_meta(client *cli, char peer_meta[21],
                            struct peer_info *peer) {
  cli->msg_buf = cli->buf;
  cli->msg_buf = encode16(cli->msg_buf, GetPeerInfoFromMeta);
  cli->msg_buf = encode8(cli->msg_buf, (uint8_t)strlen(peer_meta));
  cli->msg_buf = encode(cli->msg_buf, peer_meta, strlen(peer_meta));
  return send_get_peer_info_request(cli, peer);
}

int recv_peer_info(int fd, struct peer_info *peer) {
  struct my_peer_info peer_i;
  int n_bytes;
  n_bytes = recv(fd, (void *)&peer_i, sizeof(struct my_peer_info), 0);
  if (n_bytes <= 0) {
    return -1;
  }
  if (n_bytes == 1) {
    // offline or error
    verbose_log("It seems peer has gone offline\n");
    return 1;
  }
  verbose_log(
      "Dumping %d bytes of data received, should have dumped %ld bytes\n",
      n_bytes, sizeof(struct my_peer_info));
  hex_dump(NULL, &peer_i, n_bytes);
  peer->id = ntohl(peer_i.id);
  verbose_log("The ip got is %s\n", peer_i.ip);
  strcpy(peer->ip, peer_i.ip);
  peer->port = ntohs(peer_i.port);
  peer->type = ntohs(peer_i.type);
  verbose_log("Peer info got, id: %d, ip: %s, port: %d, type: %s, len: %d\n",
              peer->id, peer->ip, peer->port, get_nat_desc(peer->type),
              peer_i.len);
  peer->meta = malloc(peer_i.len);
  int m_bytes = recv(fd, (void *)peer->meta, peer_i.len, 0);
  if (m_bytes <= 0) {
    verbose_log("failed to get meta, error: %s\n", strerror(errno));
    return -1;
  }
  verbose_log("meta is %s\n", peer->meta);
  n_bytes += m_bytes;
  hex_dump(NULL, peer, n_bytes);
  return n_bytes;
}

int send_get_peer_info_request(client *cli, struct peer_info *peer) {
  if (-1 == send_to_punch_server(cli)) {
    return -1;
  }
  if (recv_peer_info(cli->sfd, peer) > 0) {
    return 0;
  }
  return -1;
}

static int send_dummy_udp_packet(int fd, struct sockaddr_in addr) {
  char dummy = 'c';

  struct timeval tv = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));

  return sendto(fd, &dummy, 1, 0, (struct sockaddr *)&addr, sizeof(addr));
}

static int punch_hole(struct sockaddr_in peer_addr, int ttl) {
  int hole = socket(AF_INET, SOCK_DGRAM, 0);
  if (hole != -1) {
    // struct sockaddr_in local_addr;
    // local_addr.sin_family = AF_INET;
    // local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    // local_addr.sin_port = htons(DEFAULT_LOCAL_PORT + 1);
    // int reuse_addr = 1;
    // setsockopt(hole, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse_addr,
    // sizeof(reuse_addr)); if (bind(hole, (struct sockaddr *)&local_addr,
    // sizeof(local_addr))) {
    //    if (errno == EADDRINUSE) {
    //        verbose_log("addr already in use, try another port\n");
    //        return -1;
    //    }
    //}

    /* TODO we can use traceroute to get the number of hops to the peer
     * to make sure this packet woudn't reach the peer but get through the NAT
     * in front of itself
     */
    setsockopt(hole, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl));

    // send short ttl packets to avoid triggering flooding protection of NAT in
    // front of peer
    if (send_dummy_udp_packet(hole, peer_addr) < 0) {
      return -1;
    }
  }

  return hole;
}

static int wait_for_peer(int *socks, int sock_num, struct timeval *timeout) {
  fd_set fds;
  int max_fd = 0;
  FD_ZERO(&fds);

  int i;
  for (i = 0; i < sock_num; ++i) {
    FD_SET(socks[i], &fds);
    if (socks[i] > max_fd) {
      max_fd = socks[i];
    }
  }
  int ret = select(max_fd + 1, &fds, NULL, NULL, timeout);

  int index = -1;
  if (ret > 0) {
    for (i = 0; i < sock_num; ++i) {
      if (FD_ISSET(socks[i], &fds)) {
        index = i;
        break;
      }
    }
  } else {
    // timeout or error
  }

  // one of the fds is ready, close others
  if (index != -1) {
    for (i = 0; i < sock_num; ++i) {
      if (index != i) {
        close(socks[i]);
      }
    }

    return socks[index];
  }

  return -1;
}

static void shuffle(int *num, int len) {
  srand(time(NULL));

  // Fisher-Yates shuffle algorithm
  int i, r, temp;
  for (i = len - 1; i > 0; i--) {
    r = rand() % i;

    temp = num[i];
    num[i] = num[r];
    num[r] = temp;
  }
}

static int connect_to_symmetric_nat(client *c, struct peer_info remote_peer) {
  // TODO choose port prediction strategy

  /*
   * according to birthday paradox, probability that port randomly chosen from
   * [1024, 65535] will collide with another one chosen by the same way is p(n)
   * = 1-(64511!/(64511^n*64511!)) where '!' is the factorial operator, n is the
   * number of ports chosen. P(100)=0.073898 P(200)=0.265667 P(300)=0.501578
   * P(400)=0.710488
   * P(500)=0.856122
   * P(600)=0.938839
   * but symmetric NAT has port sensitive filter for incoming packet
   * which makes the probalility decline dramatically.
   * Moreover, symmetric NATs don't really allocate ports randomly.
   */
  struct sockaddr_in peer_addr;

  peer_addr.sin_family = AF_INET;
  peer_addr.sin_addr.s_addr = inet_addr(remote_peer.ip);

  int *holes = malloc(NUM_OF_PORTS * sizeof(int));
  shuffle(ports, MAX_PORT - MIN_PORT + 1);

  int i = 0;
  for (; i < NUM_OF_PORTS;) {
    uint16_t port = ports[i];
    if (port != remote_peer.port) { // exclude the used one
      peer_addr.sin_port = htons(port);

      if ((holes[i] = punch_hole(peer_addr, c->ttl)) < 0) {
        // NAT in front of us wound't tolerate too many ports used by one
        // application
        verbose_log("failed to punch hole, error: %s\n", strerror(errno));
        break;
      }
      // sleep for a while to avoid flooding protection
      usleep(1000 * 100);
      ++i;
    } else {
      ports[i] = ports[1000];
      continue;
    }
  }

  // hole punched, notify remote peer via punch server
  c->msg_buf = encode16(c->msg_buf, NotifyPeer);
  c->msg_buf = encode32(c->msg_buf, remote_peer.id);
  send_to_punch_server(c);

  struct timeval timeout = {100, 0};
  int fd = wait_for_peer(holes, i, &timeout);
  if (fd > 0) {
    on_connected(fd);
  } else {
    int j = 0;
    for (; j < i; ++j) {
      close(holes[j]);
    }
    verbose_log("timout, not connected\n");
  }

  return 0;
}

void try_connect_to_peer(struct peer_info peer) {
  verbose_log("recved command, ready to connect to %s:%d\n", peer.ip,
              peer.port);

  struct sockaddr_in peer_addr;

  peer_addr.sin_family = AF_INET;
  peer_addr.sin_addr.s_addr = inet_addr(peer.ip);

  int sock_array[NUM_OF_PORTS];
  int i = 0;

  shuffle(ports, MAX_PORT - MIN_PORT + 1);
  // send probe packets, check if connected with peer, if yes, stop probing
  for (; i < NUM_OF_PORTS;) {
    if (ports[i] == peer.port) {
      ports[i] = ports[1000]; // TODO
      continue;
    }
    if ((sock_array[i] = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
      verbose_log("failed to create socket, send %d probe packets\n", i);
      break;
    }

    peer_addr.sin_port = htons(ports[i]);

    // let OS choose available ports
    if (send_dummy_udp_packet(sock_array[i], peer_addr) < 0) {
      verbose_log("may trigger flooding protection\n");
      break;
    }

    // wait for a while
    struct timeval tv = {0, 1000 * 100};
    int fd = wait_for_peer(sock_array, ++i, &tv);
    if (fd > 0) {
      // connected
      on_connected(fd);

      // TODO
      return;
    }
  }

  verbose_log("holes punched, waiting for peer\n");
  struct timeval tv = {100, 0};
  int fd = wait_for_peer(sock_array, i, &tv);
  if (fd > 0) {
    on_connected(fd);
  } else {
    int j = 0;
    for (j = 0; j < i; ++j) {
      close(sock_array[j]);
    }
  }
}

// run in another thread
static void *server_notify_handler(void *data) {
  int server_sock = *(int *)data;
  struct peer_info peer;

  // wait for notification
  verbose_log("waiting for notification...\n");
  for (;;) {
    struct peer_info peer;
    if (recv_peer_info(server_sock, &peer) > 0) {
      break;
    }
  }

  verbose_log("recved command, ready to connect to %s:%d\n", peer.ip,
              peer.port);

  try_connect_to_peer(peer);
  return NULL;
}

int init(struct sockaddr_in punch_server, client *c) {
  int server_sock = socket(AF_INET, SOCK_STREAM, 0);

  int res = connect(server_sock, (struct sockaddr *)&punch_server,
                    sizeof(punch_server));
  if (res < 0) {
    verbose_log("failed to connect to punch server: %d, port: %d\n",
                punch_server.sin_addr.s_addr, punch_server.sin_port);
    verbose_log("code: %d, error: %s\n", res, strerror(errno));
    return res;
  }
  c->sfd = server_sock;
  return 0;
}

int enroll(struct peer_info self, struct sockaddr_in punch_server, client *c) {
  verbose_log("enrolling, meta: %s, ip: %s, port: %d, nat type: %s\n",
              self.meta, self.ip, self.port, get_nat_desc(self.type));

  int i, temp;
  for (i = 0, temp = MIN_PORT; temp <= MAX_PORT; i++, temp++) {
    ports[i] = temp;
  }

  int res = init(punch_server, c);
  if (res != 0) {
    verbose_log("failed to connect to punch server\n");
    return res;
  }

  c->msg_buf = c->buf;
  c->msg_buf = encode16(c->msg_buf, Enroll);
  c->msg_buf = encode(c->msg_buf, self.ip, 16);
  c->msg_buf = encode16(c->msg_buf, self.port);
  c->msg_buf = encode16(c->msg_buf, self.type);
  c->msg_buf = encode8(c->msg_buf, (uint8_t)strlen(self.meta));
  c->msg_buf = encode(c->msg_buf, self.meta, strlen(self.meta));

  if (-1 == send_to_punch_server(c)) {
    verbose_log("sending to punch server failed\n");
    return -1;
  }

  // wait for server reply to get own ID
  uint32_t peer_id = 0;
  struct timeval tv;
  tv.tv_sec = 10;
  tv.tv_usec = 0;
  setsockopt(c->sfd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
  int n = recv(c->sfd, &peer_id, sizeof(uint32_t), 0);
  if (n != sizeof(uint32_t)) {
    return -1;
  }

  c->id = ntohl(peer_id);
  verbose_log("enrolled, id: %d\n", c->id);

  return 0;
}

pthread_t wait_for_command(int *server_sock) {
  // wait for command from punch server in another thread
  pthread_t thread_id;
  pthread_create(&thread_id, NULL, server_notify_handler, (void *)server_sock);

  return thread_id;
}

void on_connected(int sock) {
  char buf[MSG_BUF_SIZE] = {0};
  struct sockaddr_in remote_addr;
  socklen_t fromlen = sizeof remote_addr;
  recvfrom(sock, buf, MSG_BUF_SIZE, 0, (struct sockaddr *)&remote_addr,
           &fromlen);
  verbose_log("recv %s\n", buf);

  verbose_log("connected with peer from %s:%d\n",
              inet_ntoa(remote_addr.sin_addr), ntohs(remote_addr.sin_port));

  // restore the ttl
  int ttl = 64;
  setsockopt(sock, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl));
  sendto(sock, "hello, peer", strlen("hello, peer"), 0,
         (struct sockaddr *)&remote_addr, sizeof(remote_addr));
}

int real_connect_to_peer(client *cli, struct peer_info *peer) {
  verbose_log("connecting to id: %d, meta: %s, ip: %s, port %d, nat type: %s\n",
              peer->id, peer->meta, peer->ip, peer->port,
              get_nat_desc(peer->type));

  return connect_to_symmetric_nat(cli, *peer);
  // choose less restricted peer as initiator
  /* switch(peer.type) { */
  /*     case OpenInternet: */
  /*         // todo */
  /*         break; */
  /*     case FullCone: */
  /*         break; */
  /*     case RestricNAT: */
  /*         // todo */
  /*         break; */
  /*     case RestricPortNAT: */
  /*         // todo  */
  /*         break; */
  /*     case SymmetricNAT: */
  /*         if (cli->type == SymmetricNAT) { */
  /*           connect_to_symmetric_nat_from_meta(cli, peer_meta, peer); */
  /*         } */
  /*         else { */
  /*             // todo */
  /*         } */
  /*         break; */
  /*     default: */
  /*         verbose_log("unknown nat type\n"); */
  /*         return -1; */
  /*         // log */
  /* } */

  /* return 0; */
}

int connect_to_peer(client *cli, uint32_t peer_id) {
  struct peer_info peer;
  int n = get_peer_info(cli, peer_id, &peer);
  if (n) {
    verbose_log("get_peer_info() return %d\n", n);
    verbose_log("failed to get info of remote peer\n");

    return -1;
  }
  return real_connect_to_peer(cli, &peer);
}

int connect_to_peer_from_meta(client *cli, char *peer_meta) {
  struct peer_info peer;
  int n = get_peer_info_from_meta(cli, peer_meta, &peer);
  if (n) {
    verbose_log("get_peer_info() return %d\n", n);
    verbose_log("failed to get info of remote peer\n");
    return -1;
  }
  return real_connect_to_peer(cli, &peer);
}
