#ifndef INVENTORY_TYPES_H_
#define INVENTORY_TYPES_H_

#include <stddef.h>
#include <stdint.h>

#include "typeinfo.h"

// Read and written as "#rrggbb" by a hook in inventory.c.
typedef struct {
    uint8_t r, g, b;
} Color;

typedef enum TI_ROOT {
    ITEM_BOOK,
    ITEM_GADGET,
    ITEM_GIFT_CARD,  // No payload
} ItemKind;

typedef struct {
    char *author;
    int pages;
} Book;

typedef struct {
    double weight;
    int warranty_years;
} Gadget;

typedef struct TI_ROOT Item {
    char *name;
    ItemKind kind;
    union {
        Book book TI_ANN(JsonWhen = ITEM_BOOK);
        Gadget gadget TI_ANN(JsonWhen = ITEM_GADGET);
    } details TI_ANN(JsonTag = kind);
    Color color;
    char **tags TI_ANN(JsonArray = tag_count);
    size_t tag_count;
    struct Item *bundled_with;
    void *cache TI_ANN(JsonSkip);
} Item;

typedef struct TI_ROOT {
    Item *items TI_ANN(JsonArray = item_count);
    size_t item_count;
} Inventory;

#endif  // INVENTORY_TYPES_H_
