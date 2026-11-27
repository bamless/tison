# tison: typeinfo + json

A small C99 JSON serializer & deserializer driven by [typeinfo.c](https://github.com/bamless/typeinfo.c)
reflection data. JSON is parsed straight into reflected C objects with a bounded,
cursor-based recursive descent parser.

- `include/tison.h`, `src/tison.c`: the parser, deserializer and serializer. They depend only on `typeinfo.h`.
- `include/tison_extlib.h`, `src/tison_extlib.c`: an optional adapter for
  [extlib](https://github.com/bamless/extlib) allocators, `Array(T)` and `StringSlice`.

## How it works

C has no runtime reflection, so the library gets it from [typeinfo.c](https://github.com/bamless/typeinfo.c),
which works in two phases:

1. **Code generation, at build time.** You mark the types you want to read or write with
   `TI_ROOT`. `typeinfo_metaprogram` parses the header with libclang and writes a
   `<header>_typeinfo.c`/`.h` pair holding static tables that describe those types and everything
   they reference: sizes, member names and offsets, enum values, and the strings attached with
   `TI_ANN(...)`. Each type gets a global named `typeinfo_<TypeName>`.
2. **Runtime.** You hand `tison_deserialize` or `tison_serialize` a pointer to your object and its
   `typeinfo_<TypeName>`. The library walks the tables: it matches JSON keys to member names,
   writes values at member offsets, and reads annotations such as `JsonArray=count` or `JsonTag=kind`
   to decide how a member maps to JSON.

`TI_ROOT` and `TI_ANN` expand to nothing in a normal compile, so the annotated header stays a
plain C header.

## Setup

You need clang and the libclang development files to build the metaprogram. See typeinfo.c's
README for per-platform instructions. A typical project vendors this repository, for example
under `extern/tison`. Its dependencies, typeinfo.c and extlib, are vendored in its `extern/`
directory, so there is nothing else to fetch:

```
my_game/
├── CMakeLists.txt or Makefile
├── extern/tison/          # this repository
└── src/
    ├── game_types.h
    └── main.c
```

Annotate the types in a header:

```c
// src/game_types.h
#include <stdint.h>

#include "typeinfo.h"

typedef enum {
    WEAPON_SWORD,
    WEAPON_BOW,
} WeaponKind;

typedef struct TI_ROOT {
    char name[32] TI_ANN(JsonCStr);
    int32_t health;
    float speed;
    WeaponKind weapon;
} Player;
```

Then use the generated `typeinfo_Player`:

```c
// src/main.c
#include <stdio.h>
#include <string.h>

#include "game_types.h"
#include "game_types_typeinfo.h"
#include "tison.h"

int main(void) {
    const char *source = "{\"name\": \"Alice\", \"health\": 100, \"speed\": 5.5, "
                         "\"weapon\": \"WEAPON_BOW\"}";

    Player player = {0};
    TisonError err;
    if(tison_deserialize(source, strlen(source), &player, TI(Player), NULL, &err) != TISON_OK) {
        fprintf(stderr, "%zu:%zu: %s\n", err.line, err.column, err.message);
        return 1;
    }

    player.health -= 10;
    TisonOptions options = {.flags = TISON_FLAG_PRETTY};
    tison_serialize_to_file(stdout, &player, TI(Player), &options, &err);
    printf("\n");
    return 0;
}
```

The generated files must exist before `main.c` compiles, so the build runs the metaprogram first.

### CMake

```cmake
cmake_minimum_required(VERSION 3.16)
project(my_game C)

add_subdirectory(extern/tison)

# Writes src/game_types_typeinfo.{c,h}. Extra arguments go to the metaprogram, e.g.
# tison_typeinfo_codegen(src/game_types.h -I${PROJECT_SOURCE_DIR}/include)
tison_typeinfo_codegen(src/game_types.h)

add_executable(my_game src/main.c src/game_types_typeinfo.c)
target_link_libraries(my_game PRIVATE tison::tison)  # or tison::extlib for the extlib adapter
```

`add_subdirectory` builds `typeinfo_metaprogram` and defines `tison_typeinfo_codegen()`. The
`tison::tison` target (also available as plain `tison`) provides the include paths for `tison.h` and `typeinfo.h`. The library's own tests and
examples are only built when it is the top-level project.

| Option                   | Default           | Effect                                                |
|--------------------------|-------------------|-------------------------------------------------------|
| `TISON_TYPEINFO_CODEGEN` | `ON`              | Builds the metaprogram and `tison_typeinfo_codegen()` |
| `TISON_BUILD_TESTS`      | `ON` at top level | Builds the tests and the `test`/`test-asan` targets   |
| `TISON_BUILD_EXAMPLES`   | `ON` at top level | Builds the examples and `run-examples`                |
| `TISON_SANITIZE`         | `OFF`             | Builds `tison` and its consumers with ASan and UBSan  |
| `TISON_INSTALL`          | `ON` at top level | Generates install rules and the CMake package         |

### Make

The core is a single source file, so a plain Makefile only needs three rules:

```make
TISON     = extern/tison
CLANG_INC = $(shell clang -print-resource-dir)/include
CFLAGS    = -std=c99 -Wall -Wextra -g -I$(TISON)/include -I$(TISON)/extern/typeinfo -Isrc

my_game: src/main.c src/game_types_typeinfo.c $(TISON)/src/tison.c
	$(CC) $(CFLAGS) $^ -o $@ -lm

# Generates both files in one run. `&:` needs GNU Make 4.3 or newer.
src/game_types_typeinfo.c src/game_types_typeinfo.h &: src/game_types.h typeinfo_metaprogram
	./typeinfo_metaprogram -I$(TISON)/extern/typeinfo -I$(CLANG_INC) $< -o src/game_types_typeinfo

typeinfo_metaprogram: $(TISON)/extern/typeinfo/typeinfo_metaprogram.c
	$(CC) -O2 -I$(TISON)/extern $< -o $@ -lclang
```

The metaprogram needs `-I` for `typeinfo.h` and for clang's builtin headers, which
`clang -print-resource-dir` locates. For the extlib adapter, also compile
`$(TISON)/src/tison_extlib.c` and add `-I$(TISON)/extern`.

### Installing

tison can also be installed as a static library with its metaprogram, a CMake package and
pkg-config files:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DTISON_BUILD_TESTS=OFF -DTISON_BUILD_EXAMPLES=OFF
cmake --build build
cmake --install build --prefix /usr/local
```

The headers go to `include/tison/`. A CMake project then uses the package in place of
`add_subdirectory`, and `tison_typeinfo_codegen()` works the same:

```cmake
find_package(tison 0.1 REQUIRED)

tison_typeinfo_codegen(src/game_types.h)

add_executable(my_game src/main.c src/game_types_typeinfo.c)
target_link_libraries(my_game PRIVATE tison::tison)
```

`extlib.h` is not installed, so `tison::extlib` users add the include directory of their own
copy. The package records the clang resource directory found at build time, because the builtin
headers must match the libclang the metaprogram links against.

With Make, `tison.pc` provides the flags plus the metaprogram and clang include paths:

```make
CFLAGS += $(shell pkg-config --cflags tison)
LDLIBS += $(shell pkg-config --libs tison)
TYPEINFO_METAPROGRAM = $(shell pkg-config --variable=typeinfo_metaprogram tison)
CLANG_INC = $(shell pkg-config --variable=clang_include tison)

src/game_types_typeinfo.c src/game_types_typeinfo.h &: src/game_types.h
	$(TYPEINFO_METAPROGRAM) $(shell pkg-config --cflags-only-I tison) -I$(CLANG_INC) $< \
		-o src/game_types_typeinfo
```

### More than one header

The library compares against `typeinfo_bool` and `typeinfo_char`, so exactly one generated file
must define the builtin types. Pass `-no-builtin-types` when generating every other header, or the
link fails with duplicate symbols. Generated files that include a shared annotated header also
define its types twice, so keep the `TI_ROOT` types a program needs in one header, or in headers
that do not include each other.

## Usage

```c
#include "tison.h"
#include "tison_extlib.h"
#include "scene_typeinfo.h"

Arena arena = make_arena();
TisonExtlibContext ext = {.allocator = &arena.base};
TisonTypeHook hooks[] = {tison_extlib_array_hook(&ext), tison_extlib_string_slice_hook(&ext)};
TisonOptions options = {
    .allocator = tison_allocator_from_extlib(&ext),
    .hooks = hooks,
    .hook_count = 2,
    .flags = TISON_FLAG_IGNORE_UNKNOWN_FIELDS,
};

Scene scene = {0};
TisonError err;
if(tison_deserialize(source, source_size, &scene, TI(Scene), &options, &err) != TISON_OK) {
    fprintf(stderr, "%zu:%zu: %s%s%s\n", err.line, err.column, err.message,
            err.path[0] ? " at " : "", err.path);
}
```

The default handling for each type is documented at the top of `include/tison.h`.

## Serializing

The same reflection data and annotations drive the writer. Output goes to any `TisonSink`
(a `write(ctx, data, len)` callback), with helpers for buffers and files:

```c
size_t len;
char buf[4096];
// truncated if len >= sizeof(buf)
tison_serialize_to_buffer(buf, sizeof(buf), &len, &scene, TI(Scene), &options, &err);

options.flags |= TISON_FLAG_PRETTY;
tison_serialize_to_file(stdout, &scene, TI(Scene), &options, &err);
```

Every field is written in declaration order, except `JsonSkip` fields and `JsonArray` counts.
NULL pointers become `null`, and an enum value that matches no enumerator is an error unless the
enum is a `JsonEnumInt`. A hook takes part in writing through its `write` callback, and a
matched hook without one makes serialization fail. Floats are written with enough digits to read
back exactly (`%.9g` / `%.17g`), which, like parsing, needs `LC_NUMERIC` set to "C".

With the extlib adapter, the `Array(T)` and `StringSlice` hooks serialize too, and
`tison_sink_from_extlib(&string_buffer)` appends the output to an extlib `StringBuffer`:

```c
StringBuffer out = {0};
tison_serialize(tison_sink_from_extlib(&out), &scene, TI(Scene), &options, &err);
```

## Annotations

Annotations are attached with `TI_ANN(...)` and change how a field or type maps to JSON:

| Annotation         | Goes on                           | Effect                                        |
|--------------------|-----------------------------------|-----------------------------------------------|
| `JsonCStr`         | `char[N]` or `char *` member      | A JSON string, needed for `char[N]`           |
| `JsonArray=count`  | `T *` member                      | Owned array, length in sibling `count`        |
| `JsonTag=kind`     | union member                      | Tagged union, selected by sibling enum `kind` |
| `JsonWhen=VARIANT` | member of a `JsonTag` union       | The variant for enumerator `VARIANT`          |
| `JsonSkip`         | member, anonymous struct or union | Never read, written or freed                  |
| `JsonEnumInt`      | enum type or enum member          | A JSON integer instead of a name              |
| `JsonFlags`        | enum type or enum member          | A bit set, as an array of names               |

```c
typedef struct TI_ROOT Shape {
    ShapeKind kind;
    union {
        Sphere sphere TI_ANN(JsonWhen = SHAPE_SPHERE);
        Box box TI_ANN(JsonWhen = SHAPE_BOX);
    } shape TI_ANN(JsonTag = kind);  // {"kind": "SHAPE_BOX", "shape": {"size": ...}}
} Shape;

typedef struct TI_ROOT {
    char name[32] TI_ANN(JsonCStr);                   // "name": "cube"
    const char *path;                                 // char * is always a string
    Material *material;                               // one owned object, null gives NULL
    Vec3 *vertices TI_ANN(JsonArray = vertex_count);  // owned array, count written for you
    uint32_t vertex_count;                            // derived, must not appear in the JSON
    Shape bounds;
    GpuBuffer *gpu TI_ANN(JsonSkip);                  // never read, written or freed
} Mesh;
```

Enums are enumerator names by default, in both directions: `"kind": "SHAPE_BOX"`. Integers are
rejected, even when they equal a declared value, and writing an undeclared value is an error.
Two annotations change that, either on the enum type or on one member, which then overrides the
type:

```c
// PERM_RW comes first, so it is preferred over PERM_READ plus PERM_WRITE.
typedef enum TI_ROOT TI_ANN(JsonFlags) {
    PERM_RW = 3,
    PERM_READ = 1,
    PERM_WRITE = 2,
    PERM_EXEC = 4,
} Permissions;

typedef struct TI_ROOT {
    Permissions perms;                   // "perms": ["PERM_RW", "PERM_EXEC"], 0 is []
    ShapeKind code TI_ANN(JsonEnumInt);  // "code": 2, any value that fits the enum
} FileEntry;
```

- `JsonFlags` reads an array of names and ORs them. Writing walks the enumerators in
  declaration order and takes each one whose bits are all set and not yet covered, so declare
  composites before their parts to prefer them. Bits that no enumerator covers are an error.
  Flags enums cannot have negative enumerators or be a `JsonTag`.
- `JsonEnumInt` reads and writes a plain JSON integer, declared or not. The storage counts as
  signed when some enumerator is negative.
- A member annotation only applies to the member itself, so arrays and pointers of enums need
  the annotation on the enum type.

## Examples

- [`examples/config.c`](examples/config.c): the basics. Scalars, an enum, a nested struct, a fixed
  array, `char *` and `JsonCStr` strings. It writes the result to a file pretty-printed, reads it
  back, and frees everything with `tison_free`.
- [`examples/inventory.c`](examples/inventory.c): owned pointers, `JsonArray`, a tagged union,
  `JsonSkip`, a hook that reads and writes colors as `"#rrggbb"`, and the diagnostics for a few
  malformed documents.
- [`examples/reviews.c`](examples/reviews.c): the extlib adapter. `Array(T)` and `StringSlice`
  fields allocated from an arena, extended after parsing, then serialized into a `StringBuffer`.

```sh
cmake --build build --target run-examples
```

## Lifetimes

- Without a configured allocator, the parser uses `malloc` and `free`. A custom `TisonAllocator`
  takes `alloc(ctx, size)` and an optional `free(ctx, ptr, size)`.
- `tison_free(dst, type, &options)` releases what parsing allocated, including the partial output
  of a failed parse, and hooks release their own values through their `free` callback. Pass the
  options used for parsing. With an allocator that has no `free`, only hook callbacks run, if
  they exist.
- extlib `StringSlice` values are copied by default. With `TisonExtlibContext.borrow_strings`,
  unescaped slices point into the source buffer instead, which must then outlive them. Escaped
  slices are still decoded into the allocator, and `tison_free` frees no slices because it cannot
  tell them apart, so use borrowing with an arena.
- Destinations must be zero-initialized. A failed parse may leave them partially written.
- Bool and char detection compare against `typeinfo_bool` and `typeinfo_char`.

## Developing

You need clang and the libclang development files for the metaprogram (see typeinfo.c's README).
typeinfo.c and extlib are vendored under `extern/`.

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target test
cmake --build build --target test-asan    # ASan + UBSan, with clang, in build/asan
cmake --build build --target run-examples
```
