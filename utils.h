#include <stdio.h>

#ifdef VERBOSE
#define verbose_log(format, ...)                                               \
  do {                                                                         \
    printf(format, ##__VA_ARGS__);                                             \
  } while (0)

#define verbose_dump(...)                                                      \
  do {                                                                         \
    hex_dump(##__VA_ARGS__);                                                   \
  } while (0)
#else
#define verbose_log(format, ...)                                               \
  do {                                                                         \
  } while (0)

#define verbose_dump(...)                                                      \
  do {                                                                         \
  } while (0)
#endif

void hex_dump(char *desc, void *addr, int len);
