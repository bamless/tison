#ifndef REVIEWS_TYPES_H_
#define REVIEWS_TYPES_H_

#include "extlib.h"
#include "typeinfo.h"

typedef struct {
    StringSlice user;
    int stars;
} Review;

typedef struct TI_ROOT {
    StringSlice title;
    Array(Ext_StringSlice) authors;
    Array(Review) reviews;
} Book;

#endif  // REVIEWS_TYPES_H_
