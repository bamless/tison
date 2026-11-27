#ifndef TEST_TYPES_H_
#define TEST_TYPES_H_

#include <stdbool.h>
#include <stdint.h>

#include "extlib.h"
#include "typeinfo.h"

typedef struct TI_ROOT {
    int8_t i8;
    uint8_t u8;
    int16_t i16;
    uint16_t u16;
    int32_t i32;
    uint32_t u32;
    int64_t i64;
    uint64_t u64;
} Integers;

typedef struct TI_ROOT {
    float f;
    double d;
    bool flag;
} Scalars;

typedef enum TI_ROOT {
    MATERIAL_DIFFUSE,
    MATERIAL_METAL = 5,
    MATERIAL_GLASS = -2,
} MaterialKind;

typedef struct {
    float x, y, z;
} Vec3;

typedef struct TI_ROOT {
    char name[8] TI_ANN(JsonCStr);
    const char *path;
    MaterialKind kind;
    Vec3 albedo;
    float roughness;
} Material;

typedef struct TI_ROOT {
    int id;
    struct {
        int width;
        int height;
    };
    union {
        int as_int;
        float as_float;
    };
    int grid[2][3];
} Layout;

typedef struct TI_ROOT {
    Vec3 points[2];
    int *ptr;
    char raw[4];
    char legacy[2] TI_ANN(CStr);
    char *label TI_ANN(JsonCStr);
    unsigned char *bytes TI_ANN(JsonCStr);
} Misc;

typedef struct TI_ROOT Node {
    int value;
    struct Node *next;
} Node;

typedef struct TI_ROOT {
    Vec3 *vertices TI_ANN(JsonArray = vertex_count);
    uint32_t vertex_count;
    int *small TI_ANN(JsonArray = small_count);
    uint8_t small_count;
    char **names TI_ANN(JsonArray = name_count);
    size_t name_count;
    int **rows TI_ANN(JsonArray = row_count);
    int row_count;
    Vec3 *origin;
    int **boxed;
    void *opaque;
    float *missing TI_ANN(JsonArray = nope);
    float *bad_count TI_ANN(JsonArray = scale);
    float scale;
    int *tight TI_ANN(JsonArray = tight_count);
    int tight_count;
} Mesh;

typedef enum TI_ROOT {
    SHAPE_NONE,
    SHAPE_SPHERE,
    SHAPE_BOX,
    SHAPE_GROUP,
} ShapeKind;

typedef struct {
    float radius;
} Sphere;

typedef struct {
    Vec3 size;
} Box;

typedef struct TI_ROOT Shape {
    ShapeKind kind;
    union {
        Sphere sphere TI_ANN(JsonWhen = SHAPE_SPHERE);
        Box box TI_ANN(JsonWhen = SHAPE_BOX);
        struct Shape *child TI_ANN(JsonWhen = SHAPE_GROUP);
    } shape TI_ANN(JsonTag = kind);
    int id;
} Shape;

typedef struct TI_ROOT {
    ShapeKind kind;
    int number;
    union {
        Sphere a TI_ANN(JsonWhen = SHAPE_CONE);
    } bad_when TI_ANN(JsonTag = kind);
    union {
        Sphere a TI_ANN(JsonWhen = SHAPE_SPHERE);
        Box b TI_ANN(JsonWhen = SHAPE_SPHERE);
    } twice TI_ANN(JsonTag = kind);
    union {
        Sphere a TI_ANN(JsonWhen = SHAPE_SPHERE);
    } not_enum TI_ANN(JsonTag = number);
    union {
        Sphere a TI_ANN(JsonWhen = SHAPE_SPHERE);
    } no_field TI_ANN(JsonTag = missing);
    union {
        Sphere a;
    } untagged;
    Shape list[2];
} UnionCases;

typedef struct TI_ROOT {
    int visible;
    int *cache TI_ANN(JsonSkip);
    struct {
        int hidden_a;
        int hidden_b;
    } TI_ANN(JsonSkip);
} Skipped;

typedef struct TI_ROOT {
    Vec3 *vertices TI_ANN(JsonArray = vertex_count);
    uint32_t vertex_count;
    char **names TI_ANN(JsonArray = name_count);
    uint16_t name_count;
    Vec3 *origin;
    int **boxed;
    Shape shapes[2];
} Model;

#define BIG_F4(p)  int p##0, p##1, p##2, p##3;
#define BIG_F16(p) BIG_F4(p##0) BIG_F4(p##1) BIG_F4(p##2) BIG_F4(p##3)
#define BIG_F64(p) BIG_F16(p##0) BIG_F16(p##1) BIG_F16(p##2) BIG_F16(p##3)

typedef struct TI_ROOT {
    BIG_F64(a)
    BIG_F64(b)
    BIG_F64(c)
    BIG_F64(d)
    struct {
        BIG_F64(e)
    };
    ShapeKind kind;
    union {
        Sphere sphere TI_ANN(JsonWhen = SHAPE_SPHERE);
    } shape TI_ANN(JsonTag = kind);
} Big;

// Enum representations

// PERM_RW comes before its parts so the writer prefers it.
typedef enum TI_ROOT TI_ANN(JsonFlags){
    PERM_NONE = 0, PERM_RW = 3, PERM_READ = 1, PERM_WRITE = 2, PERM_EXEC = 4,
} Permissions;

typedef enum TI_ANN(JsonFlags) {
    SIGNED_FLAG_A = 1,
    SIGNED_FLAG_NEG = -1,
} SignedFlags;

typedef struct TI_ROOT {
    Permissions perms;
    Permissions history[2];
    Permissions raw TI_ANN(JsonEnumInt);
    MaterialKind code TI_ANN(JsonEnumInt);
    ShapeKind shapes TI_ANN(JsonFlags);
} EnumModes;

typedef struct TI_ROOT {
    ShapeKind both TI_ANN(JsonEnumInt) TI_ANN(JsonFlags);
} EnumConflict;

typedef struct TI_ROOT {
    int count TI_ANN(JsonFlags);
} EnumOnInt;

typedef struct TI_ROOT {
    SignedFlags flags;
} EnumSignedFlags;

typedef struct TI_ROOT {
    Permissions kind;
    union {
        Sphere sphere TI_ANN(JsonWhen = PERM_READ);
    } shape TI_ANN(JsonTag = kind);
} EnumFlagsTag;

typedef struct TI_ROOT {
    ShapeKind kind TI_ANN(JsonEnumInt);
    union {
        Sphere sphere TI_ANN(JsonWhen = SHAPE_SPHERE);
    } shape TI_ANN(JsonTag = kind);
} EnumIntTag;

// extlib types

typedef Ext_Array(Material) MaterialArray;

typedef struct TI_ROOT {
    Ext_StringSlice name;
    MaterialArray materials;
    Ext_Array(int) indices;
    Ext_Array(Ext_Array(float)) nested;
    Ext_Array(Ext_StringSlice) tags;
} Scene;

typedef struct TI_ROOT {
    long double wide;
    Ext_Array(long double) values;
} Wide;

#endif  // TEST_TYPES_H_
