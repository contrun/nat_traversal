#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nat_type.h"

#define DEFAULT_STUN_SERVER_PORT 3478
#define DEFAULT_LOCAL_PORT 34780

int appendToFile(char *c, nat_type type) {
  FILE *fp;
  int retval;
  fp = fopen("public_stun_list-working.txt", "a");
  if (fp == NULL)
    return -1;
  retval = fprintf(fp, "%s, %d\n", c, type);
  fclose(fp);
  return retval;
}

int main() {
  uint16_t stun_port = DEFAULT_STUN_SERVER_PORT;
  uint16_t local_port = DEFAULT_LOCAL_PORT;
  char ext_ip[16] = {0};
  uint16_t ext_port = 0;
  char local_ip[16] = "0.0.0.0";

  FILE *fp;
  char *line = NULL;
  size_t len = 0;
  ssize_t read;

  /* from https://gist.github.com/mondain/b0ec1cf5f60ae726202e */

  fp = fopen("public-stun-list.txt", "r");
  if (fp == NULL)
    exit(EXIT_FAILURE);

  while ((read = getline(&line, &len, fp)) != -1) {
    printf("Retrieved line of length %zu:\n", read);
    printf("%s\n", line);
    line[strcspn(line, "\n")] = 0;
    char *stun_server = line;
    printf("Testing stun server %s\n", stun_server);
    nat_type type = detect_nat_type(stun_server, stun_port, local_ip,
                                    local_port, ext_ip, &ext_port);
    printf("The return value is %d (%s)\n", type, get_nat_desc(type));
    if (type != 0) {
      appendToFile(stun_server, type);
    };
    ;
  }

  fclose(fp);
  if (line)
    free(line);
  exit(EXIT_SUCCESS);
}
