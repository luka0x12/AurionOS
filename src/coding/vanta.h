// src/coding/vanta.h
// Vanta 2 - a complete, self-contained scripting language for AurionOS.
//
// I built this as a production VM, not a toy stack demo: lexer, recursive
// descent compiler with real precedence, bytecode interpreter with call
// frames, refcounted strings/arrays/maps in fixed arenas, a freestanding
// soft-math library (no libc), a formatter, a disassembler and a selftest
// harness. Zero heap allocation: every byte lives in bounded pools inside
// VantaVM. If a pool is full, Vanta reports the limit and refuses to run
// on - it never corrupts memory and never grows without bound.
//
// Language surface:
//   types      nil, bool, int (i64), float (f64), string, array, map, fn
//   statements let/const, if/else, while, for(;;), for..in, break,
//              continue, return, fn, blocks, expression statements
//   operators  full C-like precedence, compound assignment, ++/--,
//              ternary, short-circuit && ||, bitwise, shifts
//   objects    a[i], m["k"], m.k shorthand read, methods (a.push, s.substr)
//   builtins   ~65 natives: io, format, string, math, array, map, rand
//   syscall    syscall(id, a, b) bridges to the vanta_syscall_hook
//
// Memory contract: single VantaVM instance, ~500 KB. vanta_run_file uses
// a static VM. Reentrant multi-VM use would require per-instance arenas,
// which AurionOS does not need - the shell runs one script at a time.

#ifndef VANTA_H
#define VANTA_H

#include <stdint.h>
#include <stdbool.h>
#include <stdalign.h>
#include <stddef.h>

#define VANTA_VERSION_MAJOR 2
#define VANTA_VERSION_MINOR 0
#define VANTA_VERSION_PATCH 0
#define VANTA_VERSION_STRING "2.0.0"

/* capacity limits (all bounded, all enforced) */

#define VANTA_MAX_SRC         65536u     /* largest source program            */
#define VANTA_MAX_TOKENS      4096u      /* token buffer                      */
#define VANTA_MAX_BYTECODE    65536u     /* code byte arena                   */
#define VANTA_MAX_CONSTS      512u       /* string constant slots             */
#define VANTA_MAX_CONST_DATA  16384u     /* string constant bytes             */
#define VANTA_MAX_STACK       1024u      /* value stack slots                 */
#define VANTA_MAX_LOCALS      1024u      /* local slots across live frames    */
#define VANTA_MAX_LOCALDESC   512u       /* compile-time local descriptors    */
#define VANTA_MAX_FRAMES      128u       /* call depth                        */
#define VANTA_MAX_GLOBALS     256u       /* global slots                      */
#define VANTA_MAX_NAMES       384u       /* interned identifier names         */
#define VANTA_NAME_MAX        32u        /* identifier length incl. NUL       */
#define VANTA_MAX_FUNCTIONS   128u       /* script-defined functions          */
#define VANTA_MAX_LINEINFO    4096u      /* bytecode offset -> source line    */
#define VANTA_MAX_LOOP_DEPTH  16u        /* nested loops                      */
#define VANTA_MAX_BLOCK_DEPTH 32u        /* nested lexical scopes             */
#define VANTA_MAX_ARGS        16u        /* call argument count               */
#define VANTA_MAX_PARAMS      8u         /* function parameter count          */

/* string pool: refcounted strings carved from a fixed arena */
#define VANTA_STR_HEADS       1024u      /* live string headers               */
#define VANTA_STR_ARENA       (128u * 1024u)
#define VANTA_STR_CLASSES     14u        /* size classes, 16B .. 4096B        */
#define VANTA_STR_MAX         4096u      /* longest single string             */

/* object pool: arrays and maps carved from a second fixed arena */
#define VANTA_ARR_HEADS       512u
#define VANTA_MAP_HEADS       128u
#define VANTA_OBJ_ARENA       (112u * 1024u)
#define VANTA_OBJ_CLASSES     16u        /* shared byte size classes          */
#define VANTA_ARR_MAX         1024u      /* elements in one array             */
#define VANTA_MAP_BUCKETS_MAX 256u

/* diagnostics */
#define VANTA_DIAG_MSG        256u       /* error message buffer              */
#define VANTA_TRACE_MAX       8u         /* frames kept in a stack trace      */
#define VANTA_SNIPPET_MAX     120u       /* source excerpt in an error        */

/* execution budget: I cap instructions per run so a runaway loop cannot
 * lock up the kernel shell. Override with vanta_set_fuel before exec. */
#define VANTA_DEFAULT_FUEL    20000000ull

/* sentinel handles */
#define VANTA_NIL_HANDLE      0xFFFFFFFFu
#define VANTA_EMPTY_BUCKET    0xFFFFFFFFu
#define VANTA_TOMB_BUCKET     0xFFFFFFFEu
#define VANTA_NO_INDEX        0xFFFFFFFFu

/* forward */
typedef struct VantaVM VantaVM;

/* result codes */

typedef enum {
    VANTA_OK = 0,
    VANTA_ERR_SYNTAX,             /* lexer/parser rejected the program       */
    VANTA_ERR_RUNTIME,            /* typed trap during execution             */
    VANTA_ERR_TYPE,               /* operand type mismatch                   */
    VANTA_ERR_STACK_OVERFLOW,     /* value stack exhausted                   */
    VANTA_ERR_OUT_OF_MEMORY,      /* a bounded pool or arena is full         */
    VANTA_ERR_DIV_BY_ZERO,        /* integer division or modulo by zero      */
    VANTA_ERR_INDEX_BOUNDS,       /* array/string index out of range         */
    VANTA_ERR_UNKNOWN_IDENT,      /* read of an undeclared name              */
    VANTA_ERR_ARITY,              /* wrong argument count                    */
    VANTA_ERR_REASSIGN_CONST,     /* write to a const binding                */
    VANTA_ERR_UNKNOWN_SYSCALL,    /* syscall hook returned unknown           */
    VANTA_ERR_FILE_NOT_FOUND,     /* run_file could not load the source      */
    VANTA_ERR_SOURCE_TOO_LARGE,   /* source exceeds VANTA_MAX_SRC            */
    VANTA_ERR_FUEL_EXHAUSTED,     /* instruction budget spent                */
    VANTA_ERR_HALT,               /* explicit exit() from script             */
    VANTA_ERR_INTERNAL            /* VM invariant violated - report as bug   */
} VantaResult;

/* value model */

typedef enum {
    V_NIL = 0,
    V_BOOL,
    V_INT,
    V_FLOAT,
    V_STR,      /* handle -> string pool   */
    V_ARR,      /* handle -> object pool   */
    V_MAP,      /* handle -> object pool   */
    V_FUNC,     /* handle -> function table */
    V_NATIVE    /* handle -> native table   */
} VantaType;

typedef struct {
    VantaType type;
    uint8_t   pad[3];
    union {
        int64_t  i;      /* V_BOOL 0/1, V_INT, V_FUNC/V_NATIVE index */
        double   f;      /* V_FLOAT                                  */
        uint32_t h;      /* V_STR/V_ARR/V_MAP pool handle            */
    } as;
} VantaValue;

#define VANTA_VALUE_NIL   ((VantaValue){ V_NIL,   {0,0,0}, { .i = 0 } })
#define VANTA_VALUE_FALSE ((VantaValue){ V_BOOL,  {0,0,0}, { .i = 0 } })
#define VANTA_VALUE_TRUE  ((VantaValue){ V_BOOL,  {0,0,0}, { .i = 1 } })

/* bytecode opcodes */

typedef enum {
    OP_NOP = 0,
    OP_HALT,             /* stop with exit code 0                             */
    OP_PUSH_NIL,
    OP_PUSH_TRUE,
    OP_PUSH_FALSE,
    OP_PUSH_INT,         /* i64 operand                                       */
    OP_PUSH_FLOAT,       /* f64 operand                                       */
    OP_PUSH_CONST,       /* u32: string constant index                        */
    OP_LOAD_LOCAL,       /* u32: absolute local slot                          */
    OP_STORE_LOCAL,      /* u32: pops top of stack into slot                  */
    OP_LOAD_GLOBAL,      /* u32: global slot                                  */
    OP_STORE_GLOBAL,     /* u32: pops top of stack into slot                  */
    OP_POP,
    OP_DUP,              /* duplicate top                                     */
    OP_DUP2,             /* duplicate top two (obj,key for subscript assign)  */
    OP_SWAP,             /* swap top two                                      */
    OP_ADD,              /* int/float arithmetic, string+array concat         */
    OP_SUB,
    OP_MUL,
    OP_DIV,              /* int/int truncates; zero divisor traps             */
    OP_MOD,              /* truncated remainder; zero divisor traps           */
    OP_NEG,
    OP_BAND,
    OP_BOR,
    OP_BXOR,
    OP_BNOT,
    OP_SHL,
    OP_SHR,              /* arithmetic shift on int                           */
    OP_EQ,
    OP_NE,
    OP_LT,
    OP_LE,
    OP_GT,
    OP_GE,
    OP_NOT,              /* logical negation -> bool                          */
    OP_INC_LOCAL,        /* u32 slot: ++ in place                             */
    OP_DEC_LOCAL,        /* u32 slot: -- in place                             */
    OP_JMP,              /* u32 absolute                                      */
    OP_JMP_FALSE,        /* u32 absolute; peek bool                           */
    OP_JMP_FALSE_POP,    /* u32 absolute; pop bool, jump if false             */
    OP_JMP_TRUE_POP,     /* u32 absolute; pop bool, jump if true              */
    OP_INDEX,            /* obj,key -> value                                  */
    OP_INDEX_SET,        /* obj,key,val -> (nothing)                          */
    OP_INDEX_SET_KEEP,   /* obj,key,val -> val                                */
    OP_CALL,             /* u32 argc                                          */
    OP_CALL_METHOD,      /* u32 argc | (u32 method << 16): recv argN -> value */
    OP_RETURN,           /* top of stack is the return value                  */
    OP_RETURN_NIL,
    OP_MAKE_FN,          /* u32 function index -> V_FUNC on stack             */
    OP_ITER_BEGIN,       /* validate iterable; push idx 0 above obj           */
    OP_ITER_NEXT,        /* u32 dest: done -> jump (2 slots stay), else elem  */
    OP_ITER_END,         /* pop idx + obj, release                            */
    OP_SYSCALL,          /* u32 argc (1..3 int args); result pushed           */
    OP_ARRAY_NEW,        /* u32 count: pop N values, push array               */
    OP_MAP_NEW,          /* u32 entries: pop (key,val) pairs, push map        */
    OP_COUNT
} VantaOp;

/* method ids (OP_CALL_METHOD dispatch) */

typedef enum {
    M_LEN = 0,
    M_PUSH, M_POP, M_SHIFT, M_UNSHIFT, M_INSERT, M_REMOVE,
    M_GET, M_SET, M_HAS, M_DELETE, M_KEYS, M_VALUES, M_CLEAR,
    M_JOIN, M_SORT, M_REVERSE, M_FILL, M_CONCAT, M_SLICE,
    M_INDEXOF, M_CONTAINS, M_UPPER, M_LOWER, M_TRIM,
    M_SUBSTR, M_STARTSWITH, M_ENDSWITH, M_REPLACE, M_REPEAT,
    M_CHARAT, M_CODEAT, M_SPLIT, M_FIND,
    M_METHOD_COUNT
} VantaMethod;

/* native builtin ids */

typedef enum {
    N_PRINT = 0, N_PRINTLN, N_DBG, N_FORMAT,
    N_LEN, N_TYPE, N_STR, N_INT, N_FLOAT, N_BOOL,
    N_ABS, N_SIGN, N_MIN, N_MAX, N_CLAMP, N_GCD, N_LCM,
    N_SQRT, N_CBRT, N_POW, N_EXP, N_LOG, N_LOG10,
    N_FLOOR, N_CEIL, N_ROUND, N_TRUNC, N_FMOD, N_HYPOT,
    N_SIN, N_COS, N_TAN, N_ASIN, N_ACOS, N_ATAN, N_ATAN2,
    N_SINH, N_COSH, N_TANH,
    N_ISNAN, N_ISINF, N_ISINT, N_ISFLOAT, N_ISSTR, N_ISARR, N_ISMAP, N_ISFN,
    N_RAND, N_RANDINT, N_RANDSEED, N_RANDCHOICE,
    N_ARRAY, N_ARRAYOF, N_MAP, N_RANGE,
    N_SORT, N_REVERSE, N_HASH, N_CHR, N_ORD,
    N_SYSCALL, N_ASSERT, N_EXIT, N_CLOCK,
    N_NATIVE_COUNT
} VantaNative;

/* token kinds (lexer) */

typedef enum {
    TK_EOF = 0, TK_NL,
    /* literals */
    TK_INT, TK_FLOAT, TK_STR, TK_CHAR,
    /* identifier */
    TK_IDENT,
    /* keywords */
    TK_K_LET, TK_K_CONST, TK_K_FN, TK_K_IF, TK_K_ELSE,
    TK_K_WHILE, TK_K_FOR, TK_K_IN, TK_K_BREAK, TK_K_CONTINUE,
    TK_K_RETURN, TK_K_TRUE, TK_K_FALSE, TK_K_NIL,
    TK_KW_COUNT,
    /* punctuation */
    TK_LPAREN, TK_RPAREN, TK_LBRACE, TK_RBRACE, TK_LBRACKET, TK_RBRACKET,
    TK_COMMA, TK_COLON, TK_SEMI, TK_DOT, TK_QUESTION,
    /* operators */
    TK_ASSIGN,
    TK_PLUS_ASSIGN, TK_MINUS_ASSIGN, TK_STAR_ASSIGN, TK_SLASH_ASSIGN, TK_PERCENT_ASSIGN,
    TK_BAND_ASSIGN, TK_BOR_ASSIGN, TK_BXOR_ASSIGN, TK_SHL_ASSIGN, TK_SHR_ASSIGN,
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PERCENT,
    TK_AMP, TK_PIPE, TK_CARET, TK_TILDE, TK_SHL, TK_SHR,
    TK_BANG, TK_AND, TK_OR,
    TK_EQ, TK_NE, TK_LT, TK_LE, TK_GT, TK_GE,
    TK_PLUSPLUS, TK_MINUSMINUS,
    TK_KW_END
} VantaTokenKind;

typedef struct {
    uint32_t kind;      /* VantaTokenKind        */
    uint32_t line;      /* 1-based               */
    uint32_t col;       /* 1-based               */
    uint32_t src_off;   /* byte offset in source (strings: content start) */
    union {
        int64_t i;      /* TK_INT                */
        double  f;      /* TK_FLOAT              */
        uint32_t len;   /* TK_STR/TK_IDENT byte length, TK_CHAR codepoint */
    } v;
} VantaToken;

/* pooled object headers */

typedef struct {
    uint32_t len;        /* bytes, excluding NUL terminator kept in storage  */
    uint32_t refs;       /* 0 = free slot; freelist links through data_off   */
    uint32_t hash;       /* FNV-1a, computed once at creation                */
    uint32_t data_off;   /* byte offset into str_arena                       */
    uint32_t size_cls;   /* size class index                                 */
    uint32_t pad;
} VantaStrHead;

typedef struct {
    uint32_t refs;       /* 0 = free slot                                    */
    uint32_t len;
    uint32_t cap;        /* elements in current storage                      */
    uint32_t data_off;   /* byte offset into obj_arena                       */
    uint32_t size_cls;
    uint32_t pad[3];
} VantaArrHead;

/* one open-addressed bucket: string key handle + value. Linear probing with
 * tombstones; EMPTY/TOMB sentinels make deletion exact.
*/
typedef struct {
    uint32_t  key;       /* string head handle or sentinel                   */
    uint32_t  pad;
    VantaValue val;
} VantaMapBucket;

typedef struct {
    uint32_t refs;       /* 0 = free slot                                    */
    uint32_t count;      /* live entries                                     */
    uint32_t cap;        /* bucket count                                     */
    uint32_t data_off;   /* byte offset into obj_arena                       */
    uint32_t size_cls;
    uint32_t pad[3];
} VantaMapHead;

/* string builder (concat working buffer) */

#define VANTA_SB_MAX 8192u

typedef struct {
    uint32_t len;
    uint32_t pad;
    char     space[VANTA_SB_MAX];
} VantaSB;

/* compile-time tables */

typedef struct {
    char     name[VANTA_NAME_MAX];
    uint32_t len;
} VantaName;

typedef struct {
    uint32_t   name_idx;   /* index into names table               */
    VantaValue value;
    uint32_t   flags;      /* 1 = const, 2 = defined               */
    uint32_t   pad;
} VantaGlobal;

typedef struct {
    uint32_t name_idx;     /* names table ("" for anonymous)   */
    uint32_t code_off;     /* bytecode entry point             */
    uint32_t nparams;
    uint32_t nlocals;      /* params + body locals             */
    uint16_t param_name[VANTA_MAX_PARAMS]; /* name idx, for errors */
    uint16_t pad;
} VantaFunction;

typedef struct {
    uint32_t name_idx;
    uint32_t slot;         /* absolute local slot index                    */
    uint32_t depth;        /* scope depth at declaration                   */
    uint32_t fn_idx;       /* owning function                              */
    uint32_t is_const;
    uint32_t pad;
} VantaLocalDesc;

typedef struct {
    /* patch lists are chains of JMP operands; each placeholder holds the
     * bytecode offset of the next patch, or NO_INDEX at the tail */
    uint32_t break_patches;
    uint32_t continue_patches;
    uint32_t continue_pc;  /* where `continue` jumps                       */
    uint32_t drop_slots;   /* iterator slots a break must pop (for..in)    */
    uint32_t loop_depth;   /* scope depth at loop entry                    */
    uint32_t fn_idx;       /* enclosing function; break cannot cross it    */
} VantaLoop;

typedef struct {
    uint32_t pc;
    uint32_t line;
} VantaLineInfo;

typedef struct {
    uint32_t off;          /* offset into const_data                       */
    uint32_t len;
} VantaConstStr;

/* diagnostics */

typedef struct {
    VantaResult result;
    uint32_t    line;
    uint32_t    col;
    char        msg[VANTA_DIAG_MSG];
    uint32_t    msg_len;
} VantaDiag;

typedef struct {
    uint32_t fn_name_idx[VANTA_TRACE_MAX];
    uint32_t line[VANTA_TRACE_MAX];
    uint32_t count;
} VantaTrace;

/* call frame */

typedef struct {
    uint32_t ret_pc;
    uint32_t locals_base;
    uint32_t stack_base;   /* value stack depth once args are consumed     */
    uint32_t fn_idx;       /* VANTA_NO_INDEX = main script frame           */
} VantaFrame;

/* the VM */

// clang-format off
struct VantaVM {
    /* hot interpreter state first: pc, sp and the pools it touches */
    uint32_t    pc;
    uint32_t    sp;
    uint32_t    locals_top;
    uint32_t    fp;                     /* frame pointer (top frame)      */
    uint64_t    fuel;                   /* remaining instruction budget   */
    int64_t     exit_code;

    uint8_t     bytecode[VANTA_MAX_BYTECODE];
    uint32_t    bc_len;

    VantaValue  stack[VANTA_MAX_STACK];
    VantaValue  locals[VANTA_MAX_LOCALS];
    VantaFrame  frames[VANTA_MAX_FRAMES];
    uint32_t    frame_count;

    /* globals and names */
    VantaGlobal globals[VANTA_MAX_GLOBALS];
    uint32_t    global_count;
    VantaName   names[VANTA_MAX_NAMES];
    uint32_t    name_count;

    /* functions and natives */
    VantaFunction functions[VANTA_MAX_FUNCTIONS];
    uint32_t      function_count;

    /* string pool */
    VantaStrHead str_heads[VANTA_STR_HEADS];
    uint32_t     str_head_free;         /* freelist head (head index)     */
    uint32_t     str_live;
    char         str_arena[VANTA_STR_ARENA];
    uint32_t     str_arena_used;
    uint32_t     str_class_free[VANTA_STR_CLASSES]; /* offset freelist    */

    /* object pool (arrays + maps share obj_arena) */
    VantaArrHead arr_heads[VANTA_ARR_HEADS];
    uint32_t     arr_head_free;
    uint32_t     arr_live;
    VantaMapHead map_heads[VANTA_MAP_HEADS];
    uint32_t     map_head_free;
    uint32_t     map_live;
    char         obj_arena[VANTA_OBJ_ARENA];
    uint32_t     obj_arena_used;
    uint32_t     obj_class_free[VANTA_OBJ_CLASSES];

    /* string builder scratch (concat paths only, never nested) */
    VantaSB      sb;

    /* PRNG: splitmix64 seeded state + xoroshiro128** working state */
    uint64_t     rng_s[2];
    uint64_t     rng_splitmix;
    uint32_t     rng_ready;

    /* compiler state */
    const char  *source;
    uint32_t     src_len;
    VantaToken   tokens[VANTA_MAX_TOKENS];
    uint32_t     token_count;
    uint32_t     cur_token;             /* parser cursor                */
    uint32_t     cur_fn;                /* function being compiled      */
    uint32_t     local_count;           /* slots in cur_fn              */
    uint32_t     scope_depth;
    VantaLocalDesc localdesc[VANTA_MAX_LOCALDESC];
    uint32_t     localdesc_count;
    VantaLoop    loops[VANTA_MAX_LOOP_DEPTH];
    uint32_t     loop_depth;
    VantaLineInfo lineinfo[VANTA_MAX_LINEINFO];
    uint32_t     lineinfo_count;
    VantaConstStr consts[VANTA_MAX_CONSTS];
    char         const_data[VANTA_MAX_CONST_DATA];
    uint32_t     const_count;
    uint32_t     const_data_len;
    uint32_t     last_line_emitted;     /* dedupe line info             */
    uint32_t     const_str_handles[VANTA_MAX_CONSTS]; /* pooled string cache */

    /* diagnostics */
    VantaDiag    diag;
    VantaTrace   trace;
    uint32_t     trace_on;              /* print each executed line     */

    char         fmt_buf[64];           /* dtoa/itoa scratch            */

    /* flags */
    uint32_t     compiled;
    uint32_t     running;
    uint32_t     pad[13];
} __attribute__((aligned(64)));

// clang-format on

/* string constants for pool classes live in the .c; header exposes API only */

VantaResult vanta_init(VantaVM *vm, const char *source, size_t len);
VantaResult vanta_compile(VantaVM *vm);
VantaResult vanta_exec(VantaVM *vm);
VantaResult vanta_run(VantaVM *vm, const char *source, size_t len);
VantaResult vanta_run_string(const char *source);
VantaResult vanta_run_file(const char *filename);
const char *vanta_strerror(VantaResult res);
const char *vanta_last_error(VantaVM *vm);
void        vanta_format_error(VantaVM *vm, char *buf, size_t cap);
void        vanta_set_fuel(VantaVM *vm, uint64_t fuel);
void        vanta_set_trace(VantaVM *vm, bool on);
size_t      vanta_disassemble(VantaVM *vm, char *buf, size_t cap);
void        vanta_dump_stats(VantaVM *vm, char *buf, size_t cap);
int         vanta_selftest(void);        /* returns failed test count    */
const char *vanta_version(void);

/* Hook for AurionOS syscalls. Implement in commands.c or syscalls.c.
 * A weak default (returns -1) lives in vanta.c for standalone builds.
*/

extern int vanta_syscall_hook(int sys_id, int64_t arg1, int64_t arg2);

/* Output sink: weak default captures into a ring buffer so selftests can
 * assert on printed text without a console. AurionOS's real c_puts wins
 * at link time as a strong symbol.
*/

extern void c_puts(const char *s);

#endif /* VANTA_H */
