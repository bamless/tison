#include <stdio.h>
#include <string.h>

#include "inventory_types.h"
#include "inventory_types_typeinfo.h"
#include "tison.h"

static const char *source =
    "{\"items\": [\n"
    "  {\"name\": \"C Programming\", \"kind\": \"ITEM_BOOK\", \"color\": \"#336699\",\n"
    "   \"details\": {\"author\": \"K&R\", \"pages\": 272}, \"tags\": [\"classic\", \"c\"]},\n"
    "  {\"details\": {\"weight\": 0.25, \"warranty_years\": 2}, \"kind\": \"ITEM_GADGET\",\n"
    "   \"name\": \"Keyboard\", \"color\": \"#ff8800\",\n"
    "   \"bundled_with\": {\"name\": \"Gift card\", \"kind\": \"ITEM_GIFT_CARD\"}}\n"
    "]}\n";

// -----------------------------------------------------------------------------
// Color hook
// -----------------------------------------------------------------------------

static bool match_color(const TisonTypeContext *ctx, void *user_data) {
    (void)user_data;
    return ctx->type == (const Type_Info *)&typeinfo_Color;
}

static int hex(char c) {
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_color(TisonParser *p, void *dst, const TisonTypeContext *ctx, void *user_data) {
    (void)ctx;
    (void)user_data;

    tison_skip_ws(p);

    TisonSpan s;
    TisonStringInfo info;
    TisonMark start = tison_mark(p);
    if(!tison_scan_string(p, &s, &info)) return false;

    const char *c = s.begin;
    bool ok = !info.has_escapes && s.end - s.begin == 7 && c[0] == '#';
    uint8_t rgb[3];
    for(int i = 0; ok && i < 3; i++) {
        int hi = hex(c[1 + 2 * i]), lo = hex(c[2 + 2 * i]);
        ok = hi >= 0 && lo >= 0;
        rgb[i] = (uint8_t)(hi * 16 + lo);
    }
    if(!ok) {
        tison_reset(p, start);
        return tison_fail(p, TISON_ERR_TYPE, "expected a color like \"#rrggbb\"");
    }

    Color *color = dst;
    color->r = rgb[0];
    color->g = rgb[1];
    color->b = rgb[2];
    return true;
}

static bool write_color(TisonWriter *w, const void *src, const TisonTypeContext *ctx,
                        void *user_data) {
    (void)ctx;
    (void)user_data;
    const Color *color = src;
    char buf[8];
    snprintf(buf, sizeof(buf), "#%02x%02x%02x", color->r, color->g, color->b);
    return tison_write_string(w, buf, 7);
}

// -----------------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------------

static void print_item(const Item *item, int indent) {
    printf("%*s%s, #%02x%02x%02x", indent, "", item->name, item->color.r, item->color.g,
           item->color.b);
    switch(item->kind) {
    case ITEM_BOOK:
        printf(", book by %s, %d pages", item->details.book.author, item->details.book.pages);
        break;
    case ITEM_GADGET:
        printf(", gadget of %.2f kg", item->details.gadget.weight);
        break;
    case ITEM_GIFT_CARD:
        printf(", gift card");
        break;
    }
    for(size_t i = 0; i < item->tag_count; i++) printf(" [%s]", item->tags[i]);
    printf("\n");
    if(item->bundled_with) print_item(item->bundled_with, indent + 2);
}

int main(void) {
    TisonOptions options = {0};
    TisonTypeHook hooks[] = {{match_color, parse_color, write_color, NULL, NULL}};
    options.hooks = hooks;
    options.hook_count = sizeof(hooks) / sizeof(hooks[0]);

    TisonError err;
    Inventory inventory = {0};
    if(tison_deserialize(source, strlen(source), &inventory, TI(Inventory), &options, &err) !=
       TISON_OK) {
        fprintf(stderr, "%zu:%zu: %s at %s\n", err.line, err.column, err.message, err.path);
        return 1;
    }

    for(size_t i = 0; i < inventory.item_count; i++) {
        print_item(&inventory.items[i], 0);
    }

    char out[1024];
    size_t len;
    tison_serialize_to_buffer(out, sizeof(out), &len, &inventory, TI(Inventory), &options, &err);
    printf("\n%s\n\n", out);
    tison_free(&inventory, TI(Inventory), &options);

    const char *broken[] = {
        "{\"items\": [{\"name\": \"x\", \"color\": \"orange\"}]}",
        "{\"items\": [{\"details\": {\"pages\": 1}}]}",
        "{\"items\": [], \"item_count\": 3}",
        "{\"items\": [{\"name\": \"x\", \"cache\": 1}]}",
        "{\"items\": [{\"name\": \"x\",}]}",
    };

    for(size_t i = 0; i < sizeof(broken) / sizeof(*broken); i++) {
        Inventory bad = {0};
        TisonErrorCode code = tison_deserialize(broken[i], strlen(broken[i]), &bad, TI(Inventory),
                                                &options, &err);
        printf("%-50s -> %s at %s, col %zu: %s\n", broken[i], tison_strerror(code),
               err.path[0] ? err.path : "(root)", err.column, err.message);
        tison_free(&bad, TI(Inventory), &options);
    }

    return 0;
}
