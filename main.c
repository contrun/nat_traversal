#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "nat_traversal.h"
#include "utils.h"

#define DEFAULT_SERVER_PORT 9988
#define MSG_BUF_SIZE 512
#define STUN_SERVER_RETRIES 3

// definition checked against extern declaration
int verbose = 0;

int main(int argc, char **argv) {
  char *stun_server = NULL;
  char local_ip[16] = "0.0.0.0";
  uint16_t stun_port = DEFAULT_STUN_SERVER_PORT;
  uint16_t local_port = DEFAULT_LOCAL_PORT;
  char *punch_server = NULL;
  char *meta = NULL;
  char *peer_meta = NULL;
  uint32_t peer_id = 0;
  int ttl = 10;
  int get_info = 0;
  int get_info_from_meta = 0;

  static char usage[] =
      "usage: [-h] [-H STUN_HOST] [-t ttl] [-P STUN_PORT] [-s punch server] "
      "[-d id] [-i SOURCE_IP] [-p SOURCE_PORT] [-v verbose]\n";
  int opt;
  while ((opt = getopt(argc, argv, "H:h:I:t:P:p:s:m:o:d:i:vzZ")) != -1) {
    switch (opt) {
    case 'h':
      printf("%s", usage);
      break;
    case 'z':
      get_info = 1;
      break;
    case 'Z':
      get_info_from_meta = 1;
      break;
    case 'H':
      stun_server = optarg;
      break;
    case 't':
      ttl = atoi(optarg);
      break;
    case 'P':
      stun_port = atoi(optarg);
      break;
    case 'p':
      local_port = atoi(optarg);
      break;
    case 's':
      punch_server = optarg;
      break;
    case 'm':
      meta = optarg;
      break;
    case 'o':
      peer_meta = optarg;
      break;
    case 'd':
      peer_id = atoi(optarg);
      break;
    case 'i':
      strncpy(local_ip, optarg, 16);
      break;
    case 'v':
      verbose = 1;
      break;
    case '?':
    default:
      printf("invalid option: %c\n", opt);
      printf("%s", usage);

      return -1;
    }
  }

  if (punch_server == NULL) {
    printf("please specify punch server\n");
    return -1;
  }

  struct sockaddr_in server_addr;
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = inet_addr(punch_server);
  server_addr.sin_port = htons(DEFAULT_SERVER_PORT);

  if (get_info) {
    client cli;
    struct peer_info peer;
    int res = init(server_addr, &cli);

    if (res) {
      printf("init punch server socket failed\n");
      return res;
    }

    if (!peer_id) {
      printf("failed to get peer_id\n");
      return -1;
    }

    int n = get_peer_info(&cli, peer_id, &peer);
    if (n) {
      verbose_log("get_peer_info() returned %d\n", n);
      printf("failed to get info of remote peer\n");
      return -1;
    }
    return 0;
  }

  if (get_info_from_meta) {
    client cli;
    struct peer_info peer;
    int res = init(server_addr, &cli);

    if (res) {
      printf("init punch server socket failed\n");
      return res;
    }

    if (peer_meta == NULL) {
      printf("failed to get peer_meta\n");
      return -1;
    }
    int n = get_peer_info_from_meta(&cli, peer_meta, &peer);
    if (n) {
      printf("failed to get info of remote peer\n");
      return -1;
    }
    return 0;
  }

  char ext_ip[16] = {0};
  uint16_t ext_port = 0;

  // TODO we should try another STUN server if failed
  int i;
  nat_type type;
  for (i = 0; i < STUN_SERVER_RETRIES; i++) {
    type = detect_nat_type(stun_server, stun_port, local_ip, local_port, ext_ip,
                           &ext_port);
    if (type != 0) {
      break;
    }
  }

  if (!ext_port) {
    return -1;
  }

  verbose_log("nat detect got ip: %s, port %d\n", ext_ip, ext_port);
  struct peer_info self;
  self.meta = malloc(32);
  strcpy(self.ip, ext_ip);
  self.port = ext_port;
  self.type = type;
  /* printf("first %s %ld\n", meta, strlen(meta)); */
  if (meta != NULL) {
    strcpy(self.meta, meta);
  } else {
    gen_random_string(self.meta, 20);
  }
  /* printf("first %s %ld\n", self.meta, strlen(self.meta)); */

  /* strncpy(self.meta, meta, strlen(meta)); */
  /* printf(""); */

  /* printf("second %s %ld\n", meta, strlen(meta)); */

  client c;
  c.type = type;
  c.ttl = ttl;
  /* printf("third %s %ld\n", self.meta, strlen(self.meta)); */

  if (enroll(self, server_addr, &c) < 0) {
    printf("failed to enroll\n");

    return -1;
  }
  verbose_log("enroll successfully, ID: %d\n", c.id);

  if (peer_id) {
    verbose_log("connecting to peer %d\n", peer_id);
    if (connect_to_peer(&c, peer_id) < 0) {
      verbose_log("failed to connect to peer %d\n", peer_id);

      return -1;
    }
  }

  if (peer_meta != NULL) {
    verbose_log("connecting to peer %s\n", peer_meta);
    if (connect_to_peer_from_meta(&c, peer_meta) < 0) {
      verbose_log("failed to connect to peer %s\n", peer_meta);

      return -1;
    }
  }

  pthread_t tid = wait_for_command(&c.sfd);

  pthread_join(tid, NULL);
  return 0;
}
