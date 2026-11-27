#ifndef CONFIG_TYPES_H_
#define CONFIG_TYPES_H_

#include <stdbool.h>
#include <stdint.h>

#include "typeinfo.h"

typedef enum TI_ROOT {
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
} LogLevel;

typedef struct {
    char host[64] TI_ANN(JsonCStr);
    uint16_t port;
} Endpoint;

typedef struct TI_ROOT {
    char *name;
    int version;
    bool verbose;
    double timeout;
    LogLevel log_level;
    Endpoint endpoint;
    int retry_delays[3];
} Config;

#endif  // CONFIG_TYPES_H_
