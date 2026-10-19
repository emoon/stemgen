# C Code Style Guide

A reusable C11 code style reference extracted from a production cross-platform codebase. Enforced by clang-format with the configuration included at the end.

---

## Formatting Fundamentals

| Rule | Value |
|------|-------|
| Indentation | 4 spaces, no tabs |
| Column limit | 120 characters |
| Brace style | K&R (Attach): opening brace on same line |
| Pointer alignment | Left: `int* p`, `void* ptr` |
| Binary operator breaks | Before the operator |
| Ternary operator breaks | Before `?` and `:` |
| Short if/loops on one line | Never |
| Short blocks on one line | Only if empty |
| Trailing comments | Aligned, 1 space before `//` |
| Between functions | One blank line, no separator lines |

---

## Header Guards

Always use `#pragma once`. Never use `#ifndef`/`#define` include guards.

```c
#pragma once

#include <stdbool.h>
#include <stdint.h>
```

---

## Include Ordering

Includes are sorted case-insensitively within groups. Groups are separated by blank lines and preserved as-is by the formatter.

### In .c files

1. Module's own header
2. Other internal/sibling headers
3. System headers

```c
#include "arena.h"
#include "base.h"
#include "base_assert.h"
#include "log.h"
#include "memory.h"
#include "os/os.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
```

### In plugin .c files

1. Public API headers via angle brackets
2. System headers

```c
#include <replay/arena.h>
#include <replay/log.h>
#include <replay/string.h>
#include <stdbool.h>
#include <stdint.h>
```

---

## Naming Conventions

### Types

PascalCase everywhere. Public API types carry a project prefix (e.g., `Rp`); internal types do not.

```c
RpArena, RpString, RpTempArena         // public
ArenaSettings, HashMapBase, LogChannel  // internal
Vec2, Ivec2, StringArray               // internal
```

### Functions

snake_case. Public/plugin API functions carry a project prefix; internal functions do not.

```c
arena_alloc_raw()      // internal
string_equals()        // internal
rp_arena_alloc()       // public API
rp_vfs_mount()         // public API
```

Static (file-private) helper functions are also snake_case with no prefix.

### Variables

All snake_case:

```c
u64 current_pos;
bool is_directory;
int64_t read_size;
```

### Globals

| Context | Prefix | Example |
|---------|--------|---------|
| Static file-scope | `s_` | `s_channel_count`, `s_log_arena` |
| Plugin module-local | `g_rp_` | `g_rp_log`, `g_rp_arena` |

### Macros

`ALL_CAPS_SNAKE_CASE` for constants and most macros. Lowercase for function-like macros that mimic control flow or generic function calls.

```c
#define ARENA_SIZE 128                          // constant
#define MAX_LOG_CHANNELS 32                     // constant
#define KB(x) ((u64)(x) * 1024ULL)             // unit helper
#define RP_VFS_HANDLE_INVALID ((RpVfsHandle)0)  // sentinel

#define for_count(i, n) for (...)               // control flow, lowercase
#define hashmap_insert(map, k, v) ({...})       // generic, lowercase
#define arena_alloc(arena, type) (...)           // function-like, lowercase
```

### Enum Values

`TypeName_ValueName` pattern. Success/zero value is always first and explicitly `= 0`.

```c
typedef enum LogLevel {
    LogLevel_Trace = 0,
    LogLevel_Debug,
    LogLevel_Info,
    LogLevel_Warning,
    LogLevel_Error,
    LogLevel_Fatal
} LogLevel;
```

---

## Type Definitions

### Structs

Always typedef in a single block. The tag name and typedef name match.

```c
typedef struct RpArena {
    void* ptr;
    u64 reserved_size;
    u64 page_size;
    struct RpArena* next;       // self-referential uses struct tag
} RpArena;
```

Anonymous structs (no self-reference needed) omit the tag:

```c
typedef struct {
    Mutex mutex;
    RpArena* head;
    RpArena* tail;
} ArenaTracker;
```

### Forward declarations

For opaque types used across compilation units:

```c
struct RpArena;
struct RpMemoryAPI;
```

### Function pointer typedefs

```c
typedef void (*ArenaIteratorCallback)(RpArena* arena, void* user_data);
typedef bool (*KeyEqualsFunc)(const void* a, const void* b, u64 size);
```

### Type aliases

Short fixed-width types used throughout:

```c
u8, u16, u32, u64       // unsigned
i8, i16, i32, i64       // signed
f32, f64                 // floating point
```

---

## Const Placement

West-const (const before the type). East-const is never used.

```c
const char* name;
const u64 aligned_pos = align_pow2(current_pos, alignment);
const u8* arena_start = (const u8*)self->ptr;

static bool compare(const void* a, const void* b, u64 size);
```

---

## Null Pointer

Always use `nullptr` (defined as `((void*)0)` for C11 compatibility). Never use `NULL`.

```c
if (ptr == nullptr) { ... }
void* data = nullptr;
```

Exception: Public API headers that must avoid macro dependencies use `0` directly.

---

## Bracing and Control Flow

### if/else

Braces always present. `else` on same line as closing brace.

```c
if (self == tracker.head) {
    tracker.head = next;
} else {
    prev->next = next;
}
```

### for

```c
for (u64 i = 0; i < count; i++) {
    process(items[i]);
}

for (Node* n = head; n != nullptr; n = n->next) {
    callback(n);
}
```

### while

```c
while (true) {
    u64 pos = atomic_load(&self->pos);
    // ...
}
```

### switch

Cases indented one level inside switch. Each case has its own `break`/`return`. Complex case bodies use braces.

```c
switch (level) {
    case LogLevel_Trace:
        return S("TRACE");
    case LogLevel_Debug:
        return S("DEBUG");
    default:
        return S("UNKNOWN");
}

switch (type) {
    case RANGE_TYPE: {
        Value* v = clone_range(self, setting->values, setting->count);
        new_setting.values = v;
        break;
    }
}
```

---

## Static and Inline

### `static`

All file-private functions are `static`. Only declare functions in headers if they are used across translation units.

```c
static u64 next_power_of_2(u64 n) { ... }
static bool validate_input(const u8* data, u32 size) { ... }
```

### `static inline`

For small performance-critical helpers (roughly < 10 lines), defined in headers.

```c
static inline void* arena_current_pos(RpArena* self) {
    u64 pos = atomic_load_explicit(&self->pos, memory_order_relaxed);
    return ((u8*)self->ptr) + pos;
}

static inline char char_to_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}
```

### Compiler-forced inline

```c
#if COMPILER_MSVC
#define ALWAYS_INLINE __forceinline
#elif COMPILER_CLANG || COMPILER_GCC
#define ALWAYS_INLINE __attribute__((always_inline)) inline
#else
#define ALWAYS_INLINE inline
#endif
```

---

## Struct Initialization

### Designated initializers (preferred everywhere)

```c
ArenaSettings settings = {
    .reserved_size = GB(2),
    .allocation_site_file = __FILE__,
    .allocation_site_line = __LINE__,
};
```

### Zero-initialization

```c
Module new_module = { 0 };
static ArenaTracker g_tracker = { 0 };
```

### Inline compound literals

```c
return (RpTempArena) {
    .arena = self,
    .pos = atomic_load_explicit(&self->pos, memory_order_relaxed),
};
```

---

## Return Style

### Early returns for guard clauses

```c
void iterate_all(Callback cb, void* user_data) {
    if (!cb) {
        return;
    }
    // ...
}
```

### Result structs for fallible operations

```c
typedef struct {
    u64 value;
    ParseStatus status;
} ParseResultU64;

ParseResultU64 parse_u64(const char* str) {
    if (str == nullptr) {
        return (ParseResultU64){ .value = 0, .status = ParseStatus_NullPointer };
    }
    // ...
}
```

### Named return variable for complex cases

```c
RpTempArena arena_scratch_begin(void) {
    RpTempArena temp;
    temp.arena = s_arena_scratch;
    temp.pos = atomic_load_explicit(&temp.arena->pos, memory_order_relaxed);
    return temp;
}
```

---

## Error Handling

Three-tier hybrid approach:

### 1. Programmer errors (preconditions): assertions

```c
static void internal_helper(RpArena* arena, u64 size) {
    assert(arena != nullptr && "Precondition: arena must not be null");
    assert(size > 0 && "Precondition: size must be positive");
    // ...
}
```

### 2. Public API validation: validate and early return

```c
void process_data(Context* ctx, const u8* data, u32 size) {
    RP_VALIDATE(ctx != nullptr, /* void */);
    RP_VALIDATE(data != nullptr, /* void */);
    // ...
}

VfsHandle open_file(VfsMount* mount, RpString path) {
    RP_VALIDATE(mount != nullptr, VFS_HANDLE_INVALID);
    RP_VALIDATE(!string_is_empty(path), VFS_HANDLE_INVALID);
    // ...
}
```

### 3. Runtime errors: result structs with status

```c
typedef struct {
    FILE* file;
    RpStatus status;
} FileOpenResult;

FileOpenResult open_file(RpString path) {
    FILE* f = fopen(path.data, "r");
    if (f == nullptr) {
        log_error("Failed to open file: %S", path);
        return (FileOpenResult){ .file = nullptr, .status = RpStatus_FileNotFound };
    }
    return (FileOpenResult){ .file = f, .status = RpStatus_Success };
}
```

### Fatal errors: log and exit

```c
if (unlikely(result != SUCCESS)) {
    log_fatal("Unable to reserve memory: %s:%d", file, line);
    log_flush();
    exit(1);
}
```

**Rule: every error must log or return error status. No silent failures.**

---

## Macro Style

### Multi-line macros

Backslash-continued, body indented, backslashes left-aligned.

```c
#define arena_new(...)            \
    arena_new_(&(ArenaSettings) { \
        .reserved_size = GB(2),   \
        .allocation_site_file = __FILE__, \
        .allocation_site_line = __LINE__, \
        __VA_ARGS__               \
    })
```

### Statement macros

Use `do { ... } while (0)` pattern:

```c
#define assert(expr)                                            \
    do {                                                        \
        if (unlikely(!(expr))) {                                \
            assert_handler(#expr, __FILE__, __LINE__, true);    \
        }                                                       \
    } while (0)
```

### Expression macros (GCC/Clang)

Use `({ ... })` statement expressions when a return value is needed:

```c
#define hashmap_insert(map, key_val, value_val)       \
    ({                                                \
        typedef __typeof__(*(map)->_key_type) _key_t; \
        /* ... */                                     \
        result_ptr;                                   \
    })
```

---

## Comment Style

### Single-line

`//` with a space after. On own line or inline.

```c
// Use atomic exchange to make sure only one thread initializes
bool expected = false;

self->key_size = key_size; // matching field
```

### Multi-line blocks

Repeated `//` lines, not `/* */`:

```c
// Automatic Cleanup Support (GCC/Clang)
//
// These macros provide RAII-style automatic cleanup for arena scopes
// using the cleanup attribute. When a variable with cleanup attribute
// goes out of scope, the cleanup function is automatically called.
```

### File/section headers

A plain comment block at the top of the file or section:

```c
// Replay String API
//
// Length-prefixed strings for safe string handling.
// Arena-based memory management for zero-copy operations.
```

### Function documentation

Brief `//` comment directly above the declaration:

```c
// Convert string to null-terminated C string (allocated from arena)
// Returns: Null-terminated string allocated from arena
const char* string_to_cstr(RpArena* arena, RpString str);
```

---

## Line Wrapping

Long function calls wrap with arguments aligned to the opening parenthesis or with 4-space continuation indent:

```c
// Aligned to opening paren
hashmap_init_raw(&(map)->base, arena_ptr, initial_capacity,
                 sizeof(_key_t), sizeof(_value_t), _node_size);

// Continuation indent for log calls
log_fatal(
    "Out of memory: %s:%d (requested: %.2f MB, committed: %.2f MB)",
    self->file, self->line, size / (1024.0 * 1024.0),
    committed / (1024.0 * 1024.0));
```

Binary operators break before the operator:

```c
bool type_ok = (req_type == reg_type)
               || ((req_type == INTEGER_TYPE || req_type == INTEGER_RANGE_TYPE)
                   && (reg_type == INTEGER_TYPE || reg_type == INTEGER_RANGE_TYPE));
```

---

## Platform Abstraction

Use platform detection macros, not raw preprocessor checks:

```c
// Good
#if PLATFORM_WINDOWS
    // ...
#elif PLATFORM_LINUX
    // ...
#elif PLATFORM_MACOS
    // ...
#endif

// Bad
#ifdef _WIN32
```

---

## clang-format Configuration

```yaml
---
Language: Cpp
BasedOnStyle: LLVM
IndentWidth: 4
TabWidth: 4
UseTab: Never
ColumnLimit: 120
BreakBeforeBraces: Attach
AlignConsecutiveAssignments: false
AlignConsecutiveDeclarations: false
AlignOperands: true
IndentCaseLabels: true
AlignTrailingComments: true
AllowAllParametersOfDeclarationOnNextLine: true
AllowShortBlocksOnASingleLine: Empty
AllowShortCaseLabelsOnASingleLine: false
AllowShortFunctionsOnASingleLine: Empty
AllowShortIfStatementsOnASingleLine: false
AllowShortLoopsOnASingleLine: false
BinPackArguments: true
BinPackParameters: true
BreakBeforeBinaryOperators: All
BreakBeforeTernaryOperators: true
BreakStringLiterals: true
DerivePointerAlignment: false
PointerAlignment: Left
SpaceAfterCStyleCast: false
SpaceBeforeAssignmentOperators: true
SpaceBeforeParens: ControlStatements
SpaceInEmptyParentheses: false
SpacesBeforeTrailingComments: 1
SpacesInAngles: false
SpacesInCStyleCastParentheses: false
SpacesInContainerLiterals: true
SpacesInParentheses: false
SpacesInSquareBrackets: false
Standard: Auto
SortIncludes: CaseInsensitive
IncludeBlocks: Preserve
AlignEscapedNewlines: Left
Cpp11BracedListStyle: false
SpaceBeforeCpp11BracedList: true
AlignConsecutiveMacros: false
MacroBlockBegin: ''
MacroBlockEnd: ''
```
