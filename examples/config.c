#include <stdio.h>
#include <string.h>

#include "config_types.h"
#include "config_types_typeinfo.h"
#include "tison.h"

static const char *source =
    "{\n"
    "  \"name\": \"example service\",\n"
    "  \"version\": 3,\n"
    "  \"verbose\": true,\n"
    "  \"timeout\": 2.5,\n"
    "  \"log_level\": \"LOG_WARN\",\n"
    "  \"endpoint\": {\"host\": \"localhost\", \"port\": 8080},\n"
    "  \"retry_delays\": [100, 200, 400]\n"
    "}\n";

int main(int argc, char **argv) {
    Config config = {0};

    TisonError err;
    if(tison_deserialize(source, strlen(source), &config, TI(Config), NULL, &err) != TISON_OK) {
        fprintf(stderr, "%zu:%zu: %s\n", err.line, err.column, err.message);
        return 1;
    }

    printf("%s v%d listening on %s:%u (timeout %.1fs, log level %d)\n", config.name, config.version,
           config.endpoint.host, config.endpoint.port, config.timeout, (int)config.log_level);

    config.version++;
    config.log_level = LOG_DEBUG;

    const char *path = argc > 1 ? argv[1] : "config.out.json";
    FILE *f = fopen(path, "w");
    if(!f) {
        perror(path);
        return 1;
    }

    TisonOptions options = {0};
    options.flags = TISON_FLAG_PRETTY;
    options.indent = 2;

    TisonErrorCode code = tison_serialize_to_file(f, &config, TI(Config), &options, &err);
    fclose(f);
    if(code != TISON_OK) {
        fprintf(stderr, "write failed at %s: %s\n", err.path, err.message);
        return 1;
    }

    char text[4096];
    f = fopen(path, "r");
    if(!f) {
        perror(path);
        return 1;
    }
    size_t len = fread(text, 1, sizeof(text), f);
    fclose(f);

    printf("wrote %s:\n%.*s\n", path, (int)len, text);

    Config reloaded = {0};
    if(tison_deserialize(text, len, &reloaded, TI(Config), NULL, &err) != TISON_OK) {
        fprintf(stderr, "%s:%zu:%zu: %s\n", path, err.line, err.column, err.message);
        return 1;
    }

    printf("reloaded: version %d, log level %d\n", reloaded.version, (int)reloaded.log_level);

    tison_free(&reloaded, TI(Config), NULL);
    tison_free(&config, TI(Config), NULL);
    return 0;
}
