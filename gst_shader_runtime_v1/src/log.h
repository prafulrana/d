// Small logging helpers shared by the runtime.
#ifndef DGST_LOG_H
#define DGST_LOG_H

#include <stdio.h>

#define LOG_INF(fmt, ...) fprintf(stdout, "[dgst] INFO  " fmt "\n", ##__VA_ARGS__)
#define LOG_WRN(fmt, ...) fprintf(stderr, "[dgst] WARN  " fmt "\n", ##__VA_ARGS__)
#define LOG_ERR(fmt, ...) fprintf(stderr, "[dgst] ERROR " fmt "\n", ##__VA_ARGS__)

#endif
