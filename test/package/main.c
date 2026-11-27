#include <stdio.h>
#include <string.h>

#include "player.h"
#include "player_typeinfo.h"
#include "tison.h"

// Round-trips one object, so a broken install fails at compile, link or run time.
int main(void) {
    const char *source = "{\"name\":\"Ada\",\"level\":3}";

    Player player = {0};
    TisonError err;
    if(tison_deserialize(source, strlen(source), &player, TI(Player), NULL, &err) != TISON_OK) {
        fprintf(stderr, "parse failed: %s\n", err.message);
        return 1;
    }

    char out[128];
    size_t len;
    TisonErrorCode code = tison_serialize_to_buffer(out, sizeof(out), &len, &player, TI(Player),
                                                    NULL, &err);
    tison_free(&player, TI(Player), NULL);
    if(code != TISON_OK || strcmp(out, source) != 0) {
        fprintf(stderr, "round trip mismatch: %s\n", out);
        return 1;
    }

    printf("ok: %s\n", out);
    return 0;
}
