#include <stdint.h>

#include "nat_type.h"

typedef struct client client;
struct client {
  int sfd;
  uint32_t id;
  char buf[128];
  // use a stack-based buffer to prevent memory allocation every time
  char *msg_buf;
  nat_type type;
  char ext_ip[16];
  uint16_t ext_port;
  // ttl of hole punching packets,
  // it should be greater than the number of hops between host to NAT of own
  // side and less than the number of hops between host to NAT of remote side,
  // so that the hole punching packets just die in the way
  int ttl;
};

struct my_peer_info {
  uint32_t id;
  char ip[16];
  uint16_t port;
  uint16_t type;
  uint8_t len;
} __attribute__((packed));

struct peer_info {
  uint32_t id;
  char ip[16];
  uint16_t port;
  uint16_t type;
  char *meta;
};

enum msg_type {
  Enroll = 0x01,
  GetPeerInfo = 0x02,
  NotifyPeer = 0x03,
  GetPeerInfoFromMeta = 0x04,
  NotifyPeerFromMeta = 0x05,
};

// public functions
int enroll(struct peer_info self, struct sockaddr_in punch_server, client *c);
pthread_t wait_for_command(int *server_sock);
int connect_to_peer(client *cli, uint32_t peer_id);
int connect_to_peer_from_meta(client *cli, char *peer_meta);
void on_connected(int sock);
int get_peer_info(client *cli, uint32_t peer_id, struct peer_info *peer);
int get_peer_info_from_meta(client *cli, char *peer_meta,
                            struct peer_info *peer);
int init(struct sockaddr_in punch_server, client *c);
void hex_dump(char *desc, void *addr, int len);
int send_get_peer_info_request(client *cli, struct peer_info *peer);
