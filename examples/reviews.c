// The extlib adapter.
// Array(T) and StringSlice fields, an arena for all allocations, and output into a StringBuffer.

#include <stdio.h>
#include <string.h>

#define EXTLIB_IMPL
#include "extlib.h"
#include "reviews_types.h"
#include "reviews_types_typeinfo.h"
#include "tison.h"
#include "tison_extlib.h"

static const char *source =
    "{\"title\": \"The Art of Computer Programming\",\n"
    " \"authors\": [\"Donald E. Knuth\"],\n"
    " \"reviews\": [{\"user\": \"ada\", \"stars\": 5}, {\"user\": \"al\\u00e1n\", \"stars\": 4}]}";

int main(void) {
    Arena arena = make_arena(.page_allocator = &ext_default_allocator.base);
    TisonExtlibContext ext = {.allocator = &arena.base};
    TisonTypeHook hooks[] = {tison_extlib_array_hook(&ext), tison_extlib_string_slice_hook(&ext)};
    TisonOptions options = {
        .allocator = tison_allocator_from_extlib(&ext),
        .hooks = hooks,
        .hook_count = sizeof(hooks) / sizeof(hooks[0]),
    };

    Book book = {0};

    TisonError err;
    if(tison_deserialize(source, strlen(source), &book, TI(Book), &options, &err) != TISON_OK) {
        fprintf(stderr, "%zu:%zu: %s\n", err.line, err.column, err.message);
        return 1;
    }
    printf(SS_Fmt " by " SS_Fmt ", %zu reviews\n", SS_Arg(book.title),
           SS_Arg(book.authors.items[0]), book.reviews.size);

    // The arrays carry the arena as their allocator, so they can keep growing.
    StringSlice review_user = SS("grace");
    Review extra = {review_user, review_user.size};
    array_push(&book.reviews, extra);

    StringBuffer out = {0};
    options.flags = TISON_FLAG_PRETTY;
    tison_serialize(tison_sink_from_extlib(&out), &book, TI(Book), &options, &err);
    printf(SB_Fmt "\n", SB_Arg(out));

    sb_free(&out);
    arena_destroy(&arena);
    return 0;
}
