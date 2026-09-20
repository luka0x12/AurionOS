// src/coding/vanta.c
// Vanta 2 - freestanding scripting VM. Zero malloc, bounded pools,
// soft-float math, refcounted strings/arrays/maps, disassembler, selftests.
// I keep the whole language in one translation unit on purpose: the kernel
// links it directly and no subsystem here is reusable outside the VM.

// VANTA is still in beta and not fully tested yet.

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>
#include "vanta.h"

_Static_assert(sizeof(VantaValue) == 16, "VantaValue must stay 16 bytes");
_Static_assert(sizeof(VantaToken) == 24, "VantaToken must stay 24 bytes");
_Static_assert(sizeof(VantaMapBucket) == 24, "map bucket layout drifted");
_Static_assert(sizeof(VantaVM) < (1024u * 1024u), "VantaVM exceeded 1 MB budget");

/* output capture
 * The weak c_puts keeps standalone/test builds linkable. When selftests run
 * they flip s_capture_on and printed text lands in s_capture instead of the
 * console, so assertions never depend on a working screen driver.
*/

static char     s_capture[16384];
static uint32_t s_capture_len = 0;
static bool     s_capture_on  = false;

__attribute__((weak)) void c_puts(const char *s) {
    (void)s; /* standalone builds: output is sinkholed unless captured */
}

__attribute__((weak)) int load_file_content(const char *fn, char *buf, int max_len) {
    (void)fn; (void)buf; (void)max_len;
    return -1;
}

/* all VM output funnels through here: capture-aware, then the real console */
static void vanta_out(VantaVM *vm, const char *s) {
    (void)vm;
    if (s_capture_on) {
        while (*s) {
            if (s_capture_len < sizeof(s_capture) - 1) {
                s_capture[s_capture_len++] = *s;
            }
            s++;
        }
        s_capture[s_capture_len] = '\0';
        return;
    }
    c_puts(s);
}

static void vanta_out_char(VantaVM *vm, char c) {
    char b[2] = { c, '\0' };
    vanta_out(vm, b);
}

/* result codes */

// wait, didn't i puted that in vanta.h?

const char *vanta_strerror(VantaResult res) {
    switch (res) {
        case VANTA_OK:                  return "OK";
        case VANTA_ERR_SYNTAX:          return "Syntax error";
        case VANTA_ERR_RUNTIME:         return "Runtime error";
        case VANTA_ERR_TYPE:            return "Type error";
        case VANTA_ERR_STACK_OVERFLOW:  return "Stack overflow";
        case VANTA_ERR_OUT_OF_MEMORY:   return "Out of memory (bounded pool exhausted)";
        case VANTA_ERR_DIV_BY_ZERO:     return "Division by zero";
        case VANTA_ERR_INDEX_BOUNDS:    return "Index out of bounds";
        case VANTA_ERR_UNKNOWN_IDENT:   return "Unknown identifier";
        case VANTA_ERR_ARITY:           return "Wrong argument count";
        case VANTA_ERR_REASSIGN_CONST:  return "Cannot assign to const";
        case VANTA_ERR_UNKNOWN_SYSCALL: return "Unknown syscall";
        case VANTA_ERR_FILE_NOT_FOUND:  return "File not found";
        case VANTA_ERR_SOURCE_TOO_LARGE:return "Source program too large";
        case VANTA_ERR_FUEL_EXHAUSTED:  return "Instruction budget exhausted (infinite loop?)";
        case VANTA_ERR_HALT:            return "Script exited via exit()";
        case VANTA_ERR_INTERNAL:        return "Internal VM error";
        default:                        return "Unknown error";
    }
}

const char *vanta_version(void) {
    return VANTA_VERSION_STRING;
}

/* opcode metadata
 * Names must match VantaOp exactly; operand sizes drive the disassembler and
 * the bytecode verifier so a corrupt code stream cannot walk out of bounds.
*/

typedef struct {
    const char *name;
    uint8_t     operand_bytes;   /* inline immediate size after the opcode byte */
} VantaOpInfo;

static const VantaOpInfo op_info[OP_COUNT] = {
    [OP_NOP]             = { "NOP",             0 },
    [OP_HALT]            = { "HALT",            0 },
    [OP_PUSH_NIL]        = { "PUSH_NIL",        0 },
    [OP_PUSH_TRUE]       = { "PUSH_TRUE",       0 },
    [OP_PUSH_FALSE]      = { "PUSH_FALSE",      0 },
    [OP_PUSH_INT]        = { "PUSH_INT",        8 },
    [OP_PUSH_FLOAT]      = { "PUSH_FLOAT",      8 },
    [OP_PUSH_CONST]      = { "PUSH_CONST",      4 },
    [OP_LOAD_LOCAL]      = { "LOAD_LOCAL",      4 },
    [OP_STORE_LOCAL]     = { "STORE_LOCAL",     4 },
    [OP_LOAD_GLOBAL]     = { "LOAD_GLOBAL",     4 },
    [OP_STORE_GLOBAL]    = { "STORE_GLOBAL",    4 },
    [OP_POP]             = { "POP",             0 },
    [OP_DUP]             = { "DUP",             0 },
    [OP_DUP2]            = { "DUP2",            0 },
    [OP_SWAP]            = { "SWAP",            0 },
    [OP_ADD]             = { "ADD",             0 },
    [OP_SUB]             = { "SUB",             0 },
    [OP_MUL]             = { "MUL",             0 },
    [OP_DIV]             = { "DIV",             0 },
    [OP_MOD]             = { "MOD",             0 },
    [OP_NEG]             = { "NEG",             0 },
    [OP_BAND]            = { "BAND",            0 },
    [OP_BOR]             = { "BOR",             0 },
    [OP_BXOR]            = { "BXOR",            0 },
    [OP_BNOT]            = { "BNOT",            0 },
    [OP_SHL]             = { "SHL",             0 },
    [OP_SHR]             = { "SHR",             0 },
    [OP_EQ]              = { "EQ",              0 },
    [OP_NE]              = { "NE",              0 },
    [OP_LT]              = { "LT",              0 },
    [OP_LE]              = { "LE",              0 },
    [OP_GT]              = { "GT",              0 },
    [OP_GE]              = { "GE",              0 },
    [OP_NOT]             = { "NOT",             0 },
    [OP_INC_LOCAL]       = { "INC_LOCAL",       4 },
    [OP_DEC_LOCAL]       = { "DEC_LOCAL",       4 },
    [OP_JMP]             = { "JMP",             4 },
    [OP_JMP_FALSE]       = { "JMP_FALSE",       4 },
    [OP_JMP_FALSE_POP]   = { "JMP_FALSE_POP",   4 },
    [OP_JMP_TRUE_POP]    = { "JMP_TRUE_POP",    4 },
    [OP_INDEX]           = { "INDEX",           0 },
    [OP_INDEX_SET]       = { "INDEX_SET",       0 },
    [OP_INDEX_SET_KEEP]  = { "INDEX_SET_KEEP",  0 },
    [OP_CALL]            = { "CALL",            4 },
    [OP_CALL_METHOD]     = { "CALL_METHOD",     4 },
    [OP_RETURN]          = { "RETURN",          0 },
    [OP_RETURN_NIL]      = { "RETURN_NIL",      0 },
    [OP_MAKE_FN]         = { "MAKE_FN",         4 },
    [OP_ITER_BEGIN]      = { "ITER_BEGIN",      0 },
    [OP_ITER_NEXT]       = { "ITER_NEXT",       4 },
    [OP_ITER_END]        = { "ITER_END",        0 },
    [OP_SYSCALL]         = { "SYSCALL",         4 },
    [OP_ARRAY_NEW]       = { "ARRAY_NEW",       4 },
    [OP_MAP_NEW]         = { "MAP_NEW",         4 }
};

/* token spelling table: every non-keyword token has a printable name for
 * parser error messages ("expected RPAREN, got IDENT").
*/

static const char *token_kind_name(uint32_t kind) {
    switch (kind) {
        case TK_EOF:        return "end of file";
        case TK_NL:         return "newline";
        case TK_INT:        return "integer literal";
        case TK_FLOAT:      return "float literal";
        case TK_STR:        return "string literal";
        case TK_CHAR:       return "character literal";
        case TK_IDENT:      return "identifier";
        case TK_K_LET:      return "'let'";
        case TK_K_CONST:    return "'const'";
        case TK_K_FN:       return "'fn'";
        case TK_K_IF:       return "'if'";
        case TK_K_ELSE:     return "'else'";
        case TK_K_WHILE:    return "'while'";
        case TK_K_FOR:      return "'for'";
        case TK_K_IN:       return "'in'";
        case TK_K_BREAK:    return "'break'";
        case TK_K_CONTINUE: return "'continue'";
        case TK_K_RETURN:   return "'return'";
        case TK_K_TRUE:     return "'true'";
        case TK_K_FALSE:    return "'false'";
        case TK_K_NIL:      return "'nil'";
        case TK_LPAREN:     return "'('";
        case TK_RPAREN:     return "')'";
        case TK_LBRACE:     return "'{'";
        case TK_RBRACE:     return "'}'";
        case TK_LBRACKET:   return "'['";
        case TK_RBRACKET:   return "']'";
        case TK_COMMA:      return "','";
        case TK_COLON:      return "':'";
        case TK_SEMI:       return "';'";
        case TK_DOT:        return "'.'";
        case TK_QUESTION:   return "'?'";
        case TK_ASSIGN:     return "'='";
        case TK_PLUS_ASSIGN:   return "'+='";
        case TK_MINUS_ASSIGN:  return "'-='";
        case TK_STAR_ASSIGN:   return "'*='";
        case TK_SLASH_ASSIGN:  return "'/='";
        case TK_PERCENT_ASSIGN:return "'%='";
        case TK_BAND_ASSIGN:   return "'&='";
        case TK_BOR_ASSIGN:    return "'|='";
        case TK_BXOR_ASSIGN:   return "'^='";
        case TK_SHL_ASSIGN:    return "'<<='";
        case TK_SHR_ASSIGN:    return "'>>='";
        case TK_PLUS:       return "'+'";
        case TK_MINUS:      return "'-'";
        case TK_STAR:       return "'*'";
        case TK_SLASH:      return "'/'";
        case TK_PERCENT:    return "'%'";
        case TK_AMP:        return "'&'";
        case TK_PIPE:       return "'|'";
        case TK_CARET:      return "'^'";
        case TK_TILDE:      return "'~'";
        case TK_SHL:        return "'<<'";
        case TK_SHR:        return "'>>'";
        case TK_BANG:       return "'!'";
        case TK_AND:        return "'&&'";
        case TK_OR:         return "'||'";
        case TK_EQ:         return "'=='";
        case TK_NE:         return "'!='";
        case TK_LT:         return "'<'";
        case TK_LE:         return "'<='";
        case TK_GT:         return "'>'";
        case TK_GE:         return "'>='";
        case TK_PLUSPLUS:   return "'++'";
        case TK_MINUSMINUS: return "'--'";
        default:            return "<unknown token>";
    }
}

/* keyword table, sorted by name for binary search. 14 keywords; a linear
 * scan would also be fine at this size but I keep the discipline uniform. */

typedef struct {
    const char *name;
    uint32_t    kind;
} VantaKeyword;

static const VantaKeyword keywords[] = {
    { "break",    TK_K_BREAK    },
    { "const",    TK_K_CONST    },
    { "continue", TK_K_CONTINUE },
    { "else",     TK_K_ELSE     },
    { "false",    TK_K_FALSE    },
    { "fn",       TK_K_FN       },
    { "for",      TK_K_FOR      },
    { "if",       TK_K_IF       },
    { "in",       TK_K_IN       },
    { "let",      TK_K_LET      },
    { "nil",      TK_K_NIL      },
    { "return",   TK_K_RETURN   },
    { "true",     TK_K_TRUE     },
    { "while",    TK_K_WHILE    }
};

#define VANTA_KEYWORD_COUNT (sizeof(keywords) / sizeof(keywords[0]))

static uint32_t keyword_lookup(const char *s, uint32_t len) {
    uint32_t lo = 0;
    uint32_t hi = (uint32_t)VANTA_KEYWORD_COUNT;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        const char *k = keywords[mid].name;
        uint32_t i = 0;
        int cmp = 0;
        while (i < len && k[i]) {
            if (s[i] != k[i]) { cmp = (unsigned char)s[i] < (unsigned char)k[i] ? -1 : 1; break; }
            i++;
        }
        if (cmp == 0) {
            if (i == len && k[i] == '\0')      cmp = 0;
            else if (i == len)                 cmp = -1;   /* token shorter than keyword */
            else if (k[i] == '\0')             cmp = 1;    /* token longer than keyword */
        }
        if (cmp == 0)  return keywords[mid].kind;
        if (cmp < 0)   hi = mid;
        else           lo = mid + 1;
    }
    return TK_IDENT;
}

/* method name table, sorted; resolved to VantaMethod at compile time so the
 * interpreter dispatches on a small integer, never on string compare. */

typedef struct {
    const char *name;
    uint32_t    id;
} VantaMethodInfo;

static const VantaMethodInfo method_table[] = {
    { "charAt",     M_CHARAT     },
    { "clear",      M_CLEAR      },
    { "codeAt",     M_CODEAT     },
    { "concat",     M_CONCAT     },
    { "contains",   M_CONTAINS   },
    { "delete",     M_DELETE     },
    { "endsWith",   M_ENDSWITH   },
    { "fill",       M_FILL       },
    { "find",       M_FIND       },
    { "get",        M_GET        },
    { "has",        M_HAS        },
    { "indexOf",    M_INDEXOF    },
    { "insert",     M_INSERT     },
    { "join",       M_JOIN       },
    { "keys",       M_KEYS       },
    { "len",        M_LEN        },
    { "lower",      M_LOWER      },
    { "pop",        M_POP        },
    { "push",       M_PUSH       },
    { "remove",     M_REMOVE     },
    { "repeat",     M_REPEAT     },
    { "replace",    M_REPLACE    },
    { "reverse",    M_REVERSE    },
    { "set",        M_SET        },
    { "shift",      M_SHIFT      },
    { "slice",      M_SLICE      },
    { "sort",       M_SORT       },
    { "split",      M_SPLIT      },
    { "startsWith", M_STARTSWITH },
    { "substr",     M_SUBSTR     },
    { "trim",       M_TRIM       },
    { "unshift",    M_UNSHIFT    },
    { "upper",      M_UPPER      },
    { "values",     M_VALUES     }
};

#define VANTA_METHOD_COUNT_TBL (sizeof(method_table) / sizeof(method_table[0]))

static uint32_t method_lookup(const char *s, uint32_t len) {
    uint32_t lo = 0;
    uint32_t hi = (uint32_t)VANTA_METHOD_COUNT_TBL;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        const char *k = method_table[mid].name;
        uint32_t i = 0;
        int cmp = 0;
        while (i < len && k[i]) {
            if (s[i] != k[i]) { cmp = (unsigned char)s[i] < (unsigned char)k[i] ? -1 : 1; break; }
            i++;
        }
        if (cmp == 0) {
            if (i == len && k[i] == '\0')      cmp = 0;
            else if (i == len)                 cmp = -1;
            else if (k[i] == '\0')             cmp = 1;
        }
        if (cmp == 0)  return method_table[mid].id;
        if (cmp < 0)   hi = mid;
        else           lo = mid + 1;
    }
    return VANTA_NO_INDEX;
}

const char *vanta_method_name(uint32_t id) {
    for (uint32_t i = 0; i < VANTA_METHOD_COUNT_TBL; i++) {
        if (method_table[i].id == id) return method_table[i].name;
    }
    return "?method";
}

/* internal forward declarations
 * Every subsystem boundary is declared here once so part ordering inside this
 * file stays flexible and no prototype is duplicated at a use site. */

static void      v_memset(void *dst, int c, size_t n);
static void      v_memcpy(void *dst, const void *src, size_t n);
static bool      v_memcmp_eq(const void *a, const void *b, size_t n);
static size_t    v_strlen(const char *s);
static size_t    v_strlcpy(char *dst, const char *src, size_t cap);
static uint32_t  v_hash_bytes(const uint8_t *data, uint32_t len);
static int       vanta_vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap);
static int       vant_snprintf(char *buf, size_t cap, const char *fmt, ...);

static uint32_t  v_fmt_i64(char *buf, int64_t val, uint32_t base, bool upper, bool sign_neg);
static uint32_t  v_fmt_f64_fixed(char *buf, size_t cap, double val, uint32_t prec);
static uint32_t  v_fmt_f64_exp(char *buf, size_t cap, double val, uint32_t prec, bool upper);
static uint32_t  v_fmt_f64_g(char *buf, size_t cap, double val, uint32_t prec, bool upper);
static double    v_parse_f64(const char *s, uint32_t len, bool *ok);
static int64_t   v_parse_i64(const char *s, uint32_t len, uint32_t base, bool *ok);

static uint32_t  utf8_decode(const char *s, uint32_t len, uint32_t *cp_out);
static uint32_t  utf8_encode(uint32_t cp, char out[4]);
static uint32_t  utf8_length(const char *s, uint32_t len);
static bool      utf8_validate(const char *s, uint32_t len);

static bool      m_isnan(double x);
static bool      m_isinf(double x);
static bool      m_finite(double x);
static double    m_fabs(double x);
static double    m_floor(double x);
static double    m_ceil(double x);
static double    m_round(double x);
static double    m_trunc(double x);
static double    m_fmod(double x, double y);
static double    m_frexp(double x, int *exp);
static double    m_ldexp(double x, int exp);
static double    m_sqrt(double x);
static double    m_cbrt(double x);
static double    m_exp(double x);
static double    m_log(double x);
static double    m_log10(double x);
static double    m_pow(double x, double y);
static double    m_sin(double x);
static double    m_cos(double x);
static double    m_tan(double x);
static double    m_atan(double x);
static double    m_atan2(double y, double x);
static double    m_asin(double x);
static double    m_acos(double x);
static double    m_sinh(double x);
static double    m_cosh(double x);
static double    m_tanh(double x);
static double    m_hypot(double x, double y);

static void      rng_seed(VantaVM *vm, uint64_t seed);
static uint64_t  rng_next_u64(VantaVM *vm);
static double    rng_next_f64(VantaVM *vm);

static bool      str_pool_init(VantaVM *vm);
static uint32_t  str_alloc(VantaVM *vm, const char *bytes, uint32_t len);
static void      str_retain(VantaVM *vm, uint32_t handle);
static void      str_release(VantaVM *vm, uint32_t handle);
static const char *str_data(VantaVM *vm, uint32_t handle);
static uint32_t  str_len(VantaVM *vm, uint32_t handle);
static uint32_t  str_hash_of(VantaVM *vm, uint32_t handle);
static bool      str_equal(VantaVM *vm, uint32_t a, uint32_t b);
static int32_t   str_compare(VantaVM *vm, uint32_t a, uint32_t b);
static void      sb_reset(VantaSB *sb);
static bool      sb_append(VantaSB *sb, const char *bytes, uint32_t len);
static bool      sb_append_char(VantaSB *sb, char c);
static bool      sb_append_value(VantaVM *vm, VantaSB *sb, VantaValue v);
static uint32_t  str_take_sb(VantaVM *vm, VantaSB *sb);

static bool      obj_pool_init(VantaVM *vm);
static uint32_t  arr_new(VantaVM *vm, uint32_t cap_hint);
static void      arr_retain(VantaVM *vm, uint32_t handle);
static void      arr_release(VantaVM *vm, uint32_t handle);
static uint32_t  arr_len(VantaVM *vm, uint32_t handle);
static bool      arr_get(VantaVM *vm, uint32_t handle, uint32_t idx, VantaValue *out);
static bool      arr_set(VantaVM *vm, uint32_t handle, uint32_t idx, VantaValue v);
static bool      arr_push(VantaVM *vm, uint32_t handle, VantaValue v);
static bool      arr_pop(VantaVM *vm, uint32_t handle, VantaValue *out);
static bool      arr_insert(VantaVM *vm, uint32_t handle, uint32_t idx, VantaValue v);
static bool      arr_remove_at(VantaVM *vm, uint32_t handle, uint32_t idx, VantaValue *out);
static uint32_t  arr_concat(VantaVM *vm, uint32_t a, uint32_t b);
static uint32_t  arr_slice(VantaVM *vm, uint32_t handle, int64_t from, int64_t to, bool *ok);
static void      arr_sort(VantaVM *vm, uint32_t handle);
static void      arr_reverse(VantaVM *vm, uint32_t handle);

static uint32_t  map_new(VantaVM *vm, uint32_t cap_hint);
static void      map_retain(VantaVM *vm, uint32_t handle);
static void      map_release(VantaVM *vm, uint32_t handle);
static uint32_t  map_count(VantaVM *vm, uint32_t handle);
static bool      map_get(VantaVM *vm, uint32_t handle, uint32_t key, VantaValue *out, bool *found);
static bool      map_set(VantaVM *vm, uint32_t handle, uint32_t key, VantaValue v);
static bool      map_delete(VantaVM *vm, uint32_t handle, uint32_t key,
                            bool *found, VantaValue *out);

static void      value_retain(VantaVM *vm, VantaValue v);
static void      value_release(VantaVM *vm, VantaValue v);
static bool      value_truthy(VantaVM *vm, VantaValue v);
static bool      value_equal(VantaVM *vm, VantaValue a, VantaValue b);
static int       value_compare(VantaVM *vm, VantaValue a, VantaValue b, bool *ok);
static const char *value_type_name(VantaValue v);
static void      value_to_sb(VantaVM *vm, VantaSB *sb, VantaValue v);
static VantaValue make_int(int64_t v);
static VantaValue make_float(double v);
static VantaValue make_bool(bool v);
static VantaValue make_str(uint32_t handle);
static VantaValue make_arr(uint32_t handle);
static VantaValue make_map(uint32_t handle);
static VantaValue make_func(uint32_t idx);
static VantaValue make_native(uint32_t idx);
static VantaValue str_value_from(VantaVM *vm, const char *bytes, uint32_t len);

static VantaResult lex_program(VantaVM *vm);

static VantaResult add_const_string(VantaVM *vm, const char *bytes, uint32_t len, uint32_t *idx_out);
static VantaResult compile_program(VantaVM *vm);

static VantaResult interpret(VantaVM *vm);

static const char *native_name(uint32_t id);
static bool       native_bounds(uint32_t id, uint32_t *min_a, uint32_t *max_a);
static VantaResult call_native(VantaVM *vm, uint32_t id, uint32_t argc,
                               VantaValue *argv, VantaValue *out);
static VantaResult call_method(VantaVM *vm, uint32_t method, VantaValue recv,
                               uint32_t argc, VantaValue *argv, VantaValue *out);

static VantaResult rt_error(VantaVM *vm, VantaResult code, const char *fmt, ...);

/* P02 - memory, string utilities and the formatter engine
 * No libc exists in the kernel image, so every primitive the VM needs is
 * implemented here on raw bytes. The formatter is the single printf-like
 * engine used by diagnostics, builtins, disassembly and error rendering -
 * one implementation, one behavior, all call sites agree. */

static void v_memset(void *dst, int c, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    /* align to 8 bytes, then bulk-fill: the arenas are memset on init */
    while (n && ((uintptr_t)d & 7u)) { *d++ = (uint8_t)c; n--; }
    if (n >= 8) {
        uint64_t rep = (uint8_t)c;
        rep |= rep << 8; rep |= rep << 16; rep |= rep << 32;
        uint64_t *d64 = (uint64_t *)d;
        size_t chunks = n / 8;
        for (size_t i = 0; i < chunks; i++) d64[i] = rep;
        d += chunks * 8;
        n &= 7u;
    }
    while (n--) *d++ = (uint8_t)c;
}

static void v_memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d == s || n == 0) return;
    if (d < s) {
        while (n && ((uintptr_t)d & 7u)) { *d++ = *s++; n--; }
        if (n >= 8) {
            const uint64_t *s64 = (const uint64_t *)s;
            uint64_t *d64 = (uint64_t *)d;
            size_t chunks = n / 8;
            for (size_t i = 0; i < chunks; i++) d64[i] = s64[i];
            d += chunks * 8; s += chunks * 8; n &= 7u;
        }
        while (n--) *d++ = *s++;
    } else {
        /* backwards copy: regions may overlap when array elements shift */
        d += n; s += n;
        while (n--) *--d = *--s;
    }
}

static bool v_memcmp_eq(const void *a, const void *b, size_t n) {
    const uint8_t *x = (const uint8_t *)a;
    const uint8_t *y = (const uint8_t *)b;
    if (x == y) return true;
    while (n && ((uintptr_t)x & 7u) == ((uintptr_t)y & 7u)) {
        if (*x != *y) return false;
        x++; y++; n--;
    }
    if (n >= 8 && (((uintptr_t)x & 7u) == 0)) {
        const uint64_t *x64 = (const uint64_t *)x;
        const uint64_t *y64 = (const uint64_t *)y;
        size_t chunks = n / 8;
        for (size_t i = 0; i < chunks; i++) {
            if (x64[i] != y64[i]) return false;
        }
        x += chunks * 8; y += chunks * 8; n &= 7u;
    }
    while (n--) { if (*x++ != *y++) return false; }
    return true;
}

static int v_memcmp_ord(const void *a, const void *b, size_t n) {
    const uint8_t *x = (const uint8_t *)a;
    const uint8_t *y = (const uint8_t *)b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    }
    return 0;
}

static size_t v_strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static size_t v_strlcpy(char *dst, const char *src, size_t cap) {
    if (cap == 0) return v_strlen(src);
    size_t i = 0;
    while (src[i] && i + 1 < cap) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
    return i;
}

/* FNV-1a over raw bytes. Deterministic, no rand(), fine for bucket scatter. */
static uint32_t v_hash_bytes(const uint8_t *data, uint32_t len) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < len; i++) {
        h ^= data[i];
        h *= 16777619u;
    }
    /* final mix: FNV's low bits are weak for power-of-two bucket counts */
    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    return h;
}

/* formatter: vant_vsnprintf
 * Supports: %[flags][width][.prec][l]{d,i,u,x,X,o,b,c,s,f,e,g,%}
 * flags: '-' left align, '0' zero pad, '+' force sign, ' ' space sign
 * 'l'/'ll'/'h' accepted and ignored: integers are always int64 here.
 * Returns the number of chars that WOULD have been written (excl NUL), so
 * callers can detect truncation. Never writes past cap. Never recurses. */

typedef struct {
    char    *buf;
    size_t   cap;
    size_t   len;      /* chars written so far (excl NUL)   */
    size_t   total;    /* chars that would be written       */
    bool     truncated;
} FmtCtx;

static void fmt_putc(FmtCtx *c, char ch) {
    if (c->len + 1 < c->cap) {
        c->buf[c->len] = ch;
    }
    c->len++;
    c->total++;
    if (c->len >= c->cap) c->truncated = true;
}

static void fmt_putn(FmtCtx *c, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) fmt_putc(c, s[i]);
}

static void fmt_puts(FmtCtx *c, const char *s) {
    while (*s) fmt_putc(c, *s++);
}

static void fmt_pad(FmtCtx *c, char fill, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) fmt_putc(c, fill);
}

/* scratch big enough for 64-bit binary + sign */
#define FMT_NUMSCRATCH 72

static uint64_t fmt_u64_from_va(va_list *ap) {
    return (uint64_t)va_arg(*ap, long long);
}

static void fmt_integer(FmtCtx *c, uint64_t raw, bool is_signed, uint32_t base,
                        bool upper, uint32_t width, uint32_t prec, bool left,
                        bool zero, bool plus, bool space, bool has_prec) {
    char digits[FMT_NUMSCRATCH];
    uint32_t nd = 0;
    bool neg = false;
    int64_t sv = (int64_t)raw;

    if (is_signed && sv < 0) {
        neg = true;
        /* two's-complement negate without UB on INT64_MIN */
        raw = (uint64_t)(-(sv + 1)) + 1ull;
    }

    static const char lowerhex[] = "0123456789abcdef";
    static const char upperhex[] = "0123456789ABCDEF";
    const char *tbl = upper ? upperhex : lowerhex;

    if (raw == 0) {
        /* precision 0 with value 0 prints nothing (C99 rule) */
        if (!(has_prec && prec == 0)) digits[nd++] = '0';
    } else {
        while (raw) {
            digits[nd++] = tbl[raw % base];
            raw /= base;
        }
    }

    uint32_t sign_w = 0;
    char sign_c = 0;
    if (neg)          { sign_c = '-'; sign_w = 1; }
    else if (plus)    { sign_c = '+'; sign_w = 1; }
    else if (space)   { sign_c = ' '; sign_w = 1; }

    uint32_t num_w = nd;
    if (has_prec && prec > num_w) num_w = prec;
    uint32_t total = sign_w + num_w;
    uint32_t fill_w = width > total ? width - total : 0;

    if (!left && !zero) fmt_pad(c, ' ', fill_w);
    if (sign_w) fmt_putc(c, sign_c);
    if (!left && zero) fmt_pad(c, '0', fill_w);
    if (has_prec && prec > nd) fmt_pad(c, '0', prec - nd);
    for (uint32_t i = 0; i < nd; i++) fmt_putc(c, digits[nd - 1 - i]);
    if (left) fmt_pad(c, ' ', fill_w);
}

static int vanta_vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap) {
    FmtCtx ctx;
    ctx.buf = buf;
    ctx.cap = cap;
    ctx.len = 0;
    ctx.total = 0;
    ctx.truncated = false;
    if (cap > 0) buf[0] = '\0';

    while (*fmt) {
        if (*fmt != '%') { fmt_putc(&ctx, *fmt++); continue; }
        fmt++;

        /* flags */
        bool left = false, zero = false, plus = false, space = false, alt = false;
        bool parsing_flags = true;
        while (parsing_flags) {
            switch (*fmt) {
                case '-': left = true;  fmt++; break;
                case '0': zero = true;  fmt++; break;
                case '+': plus = true;  fmt++; break;
                case ' ': space = true; fmt++; break;
                case '#': alt = true;   fmt++; break;
                default:  parsing_flags = false; break;
            }
        }
        if (left) zero = false;

        /* width */
        uint32_t width = 0;
        if (*fmt == '*') {
            int w = va_arg(ap, int);
            if (w < 0) { left = true; w = -w; }
            width = (uint32_t)w;
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (uint32_t)(*fmt - '0'); fmt++; }
        }

        /* precision */
        int prec = -1;
        bool has_prec = false;
        if (*fmt == '.') {
            fmt++;
            has_prec = true;
            prec = 0;
            if (*fmt == '*') {
                prec = va_arg(ap, int);
                if (prec < 0) { prec = 0; has_prec = false; }
                fmt++;
            } else {
                while (*fmt >= '0' && *fmt <= '9') { prec = prec * 10 + (*fmt - '0'); fmt++; }
            }
        }

        /* length modifiers - int64 is the only integer model here */
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'L' || *fmt == 'z' || *fmt == 'j' || *fmt == 't') fmt++;

        char conv = *fmt;
        if (conv == '\0') break;
        fmt++;

        switch (conv) {
            case 'd': case 'i':
                fmt_integer(&ctx, fmt_u64_from_va(&ap), true, 10, false,
                            width, has_prec ? (uint32_t)prec : 0, left, zero,
                            plus, space, has_prec);
                break;
            case 'u':
                fmt_integer(&ctx, fmt_u64_from_va(&ap), false, 10, false,
                            width, has_prec ? (uint32_t)prec : 0, left, zero,
                            plus, space, has_prec);
                break;
            case 'x': case 'X': {
                uint64_t raw = fmt_u64_from_va(&ap);
                if (alt && raw) fmt_puts(&ctx, conv == 'x' ? "0x" : "0X");
                fmt_integer(&ctx, raw, false, 16, conv == 'X',
                            width > (alt && raw ? 2u : 0u) ? width - (alt && raw ? 2u : 0u) : 0,
                            has_prec ? (uint32_t)prec : 0, left, zero,
                            false, false, has_prec);
                break;
            }
            case 'o': {
                uint64_t raw = fmt_u64_from_va(&ap);
                if (alt && raw) fmt_putc(&ctx, '0');
                fmt_integer(&ctx, raw, false, 8, false,
                            width > (alt && raw ? 1u : 0u) ? width - (alt && raw ? 1u : 0u) : 0,
                            has_prec ? (uint32_t)prec : 0, left, zero,
                            false, false, has_prec);
                break;
            }
            case 'b': {
                uint64_t raw = fmt_u64_from_va(&ap);
                if (alt && raw) fmt_puts(&ctx, "0b");
                fmt_integer(&ctx, raw, false, 2, false,
                            width > (alt && raw ? 2u : 0u) ? width - (alt && raw ? 2u : 0u) : 0,
                            has_prec ? (uint32_t)prec : 0, left, zero,
                            false, false, has_prec);
                break;
            }
            case 'c': {
                int ch = va_arg(ap, int);
                uint32_t w = width > 1 ? width - 1 : 0;
                if (!left) fmt_pad(&ctx, ' ', w);
                fmt_putc(&ctx, (char)ch);
                if (left) fmt_pad(&ctx, ' ', w);
                break;
            }
            case 's': {
                const char *s = va_arg(ap, const char *);
                if (!s) s = "(null)";
                size_t slen = v_strlen(s);
                if (has_prec && (size_t)prec < slen) slen = (size_t)prec;
                uint32_t fill = width > (uint32_t)slen ? width - (uint32_t)slen : 0;
                if (!left) fmt_pad(&ctx, ' ', fill);
                fmt_putn(&ctx, s, slen);
                if (left) fmt_pad(&ctx, ' ', fill);
                break;
            }
            case 'f': case 'F':
            case 'e': case 'E':
            case 'g': case 'G': {
                double val = va_arg(ap, double);
                char numbuf[64];
                bool is_nan = m_isnan(val);
                bool is_inf = m_isinf(val);
                if (is_nan || is_inf) {
                    /* sign handling for nan/inf matches the numeric path */
                    char tmp[8];
                    uint32_t tn = 0;
                    if ((is_inf && val < 0) || (is_nan && val < 0)) tmp[tn++] = '-';
                    else if (plus) tmp[tn++] = '+';
                    else if (space) tmp[tn++] = ' ';
                    const char *word = is_nan ? ((conv == 'F' || conv == 'E' || conv == 'G') ? "NAN" : "nan")
                                              : ((conv == 'F' || conv == 'E' || conv == 'G') ? "INF" : "inf");
                    for (uint32_t i = 0; word[i]; i++) tmp[tn++] = word[i];
                    tmp[tn] = '\0';
                    (void)numbuf;
                    uint32_t fill = width > tn ? width - tn : 0;
                    if (!left) fmt_pad(&ctx, ' ', fill);
                    fmt_putn(&ctx, tmp, tn);
                    if (left) fmt_pad(&ctx, ' ', fill);
                    break;
                }
                int p = has_prec ? prec : 6;
                uint32_t n;
                if (conv == 'f' || conv == 'F') {
                    n = v_fmt_f64_fixed(numbuf, sizeof(numbuf), val, (uint32_t)p);
                } else if (conv == 'e' || conv == 'E') {
                    n = v_fmt_f64_exp(numbuf, sizeof(numbuf), val, (uint32_t)p, conv == 'E');
                } else {
                    n = v_fmt_f64_g(numbuf, sizeof(numbuf), val, p == 6 ? 12u : (uint32_t)p, conv == 'G');
                }
                (void)n;
                uint32_t len = (uint32_t)v_strlen(numbuf);
                uint32_t fill = width > len ? width - len : 0;
                if (!left && !zero) fmt_pad(&ctx, ' ', fill);
                if (!left && zero) fmt_pad(&ctx, '0', fill);
                fmt_putn(&ctx, numbuf, len);
                if (left) fmt_pad(&ctx, ' ', fill);
                break;
            }
            case 'n': {
                int *np = va_arg(ap, int *);
                if (np) *np = (int)ctx.total;
                break;
            }
            case '%':
                fmt_putc(&ctx, '%');
                break;
            default:
                /* unknown conversion: echo it verbatim instead of UB */
                fmt_putc(&ctx, '%');
                fmt_putc(&ctx, conv);
                break;
        }
    }

    if (cap > 0) {
        size_t term = ctx.len < cap ? ctx.len : cap - 1;
        buf[term] = '\0';
    }
    return (int)ctx.total;
}

static int vant_snprintf(char *buf, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vanta_vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return n;
}

/* small bounded helpers used across the VM */

static bool bounded_ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static bool ident_start_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool is_space_char(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f';
}

static char lower_ascii(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

static char upper_ascii(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - ('a' - 'A')) : c;
}

/* memmem: naive scan. Longest needles in this VM are builtin names (<16B)
 * and user strings are bounded by VANTA_STR_MAX, so worst case stays tiny. */
static const char *v_memmem(const char *hay, uint32_t hay_len,
                            const char *needle, uint32_t nd_len) {
    if (nd_len == 0) return hay;
    if (nd_len > hay_len) return NULL;
    for (uint32_t i = 0; i + nd_len <= hay_len; i++) {
        if (v_memcmp_eq(hay + i, needle, nd_len)) return hay + i;
    }
    return NULL;
}

static int64_t i64_min(int64_t a, int64_t b) { return a < b ? a : b; }
static int64_t i64_max(int64_t a, int64_t b) { return a > b ? a : b; }
static uint32_t u32_min(uint32_t a, uint32_t b) { return a < b ? a : b; }
static uint32_t u32_max(uint32_t a, uint32_t b) { return a > b ? a : b; }
static int64_t i64_clamp(int64_t v, int64_t lo, int64_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* P03 - number formatting and parsing, freestanding
 * There is no printf and no strtod in the kernel, so I implement the real
 * thing: dtoa produces EXACT correct digits from the binary double via a
 * fixed-size limb bignum (no rounding-mode surprises, correct at 1e308 and
 * 5e-324 alike), and parsing is checked - overflow never wraps silently. */

static const double pow10_exact[23] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,
    1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19,
    1e20, 1e21, 1e22
};

static double bits_to_f64(uint64_t bits) {
    union { uint64_t u; double d; } cvt;
    cvt.u = bits;
    return cvt.d;
}

static uint64_t f64_to_bits(double d) {
    union { uint64_t u; double d; } cvt;
    cvt.d = d;
    return cvt.u;
}

#define F64_SIGN_BIT  0x8000000000000000ull
#define F64_EXP_MASK  0x7FF0000000000000ull
#define F64_EXP_SHIFT 52
#define F64_MANT_MASK 0x000FFFFFFFFFFFFFull
#define F64_BIAS      1023
#define F64_INF_BITS  0x7FF0000000000000ull
#define F64_QNAN_BITS 0x7FF8000000000000ull

static double make_inf(bool negative) {
    return bits_to_f64(F64_INF_BITS | (negative ? F64_SIGN_BIT : 0));
}

static double make_nan(void) {
    return bits_to_f64(F64_QNAN_BITS);
}

/* integer formatting */

/* Writes val in the given base (2,8,10,16) into buf; returns digit count.
 * buf must have room for 64 chars + sign + NUL. sign_neg forces a leading
 * '-' even for unsigned callers that pre-negated. */
static uint32_t v_fmt_i64(char *buf, int64_t val, uint32_t base, bool upper, bool sign_neg) {
    char tmp[72];
    uint32_t n = 0;
    uint64_t u;
    bool neg = sign_neg;

    if (sign_neg) {
        u = (uint64_t)(-(val + 1)) + 1ull;   /* safe even for INT64_MIN */
    } else if (val < 0) {
        neg = true;
        u = (uint64_t)(-(val + 1)) + 1ull;
    } else {
        u = (uint64_t)val;
    }

    static const char lowert[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    static const char uppert[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    const char *tbl = upper ? uppert : lowert;

    if (base < 2 || base > 36) base = 10;

    if (u == 0) {
        tmp[n++] = '0';
    } else {
        while (u) {
            tmp[n++] = tbl[u % base];
            u /= base;
        }
    }

    uint32_t out = 0;
    if (neg) buf[out++] = '-';
    while (n) buf[out++] = tmp[--n];
    buf[out] = '\0';
    return out;
}

/* fixed-size bignum for exact binary->decimal conversion
 * Limb array of uint32, little endian. 128 limbs = 4096 bits covers the
 * widest intermediate (m * 2^1024 needs ~3467 bits). All operations here are
 * single-precision multiply/divide by a small operand, which is all exact
 * dtoa needs. No heap, no recursion. */

#define BN_LIMBS 128

typedef struct {
    uint32_t v[BN_LIMBS];
    uint32_t n;          /* limbs in use, v[n..] == 0 */
} Bignum;

static void bn_zero(Bignum *b) {
    for (uint32_t i = 0; i < BN_LIMBS; i++) b->v[i] = 0;
    b->n = 0;
}

static void bn_set_u64(Bignum *b, uint64_t x) {
    bn_zero(b);
    b->v[0] = (uint32_t)(x & 0xFFFFFFFFu);
    b->v[1] = (uint32_t)(x >> 32);
    b->n = (b->v[1] ? 2u : (b->v[0] ? 1u : 0u));
}

static void bn_trim(Bignum *b) {
    while (b->n > 0 && b->v[b->n - 1] == 0) b->n--;
}

static bool bn_is_zero(const Bignum *b) {
    return b->n == 0;
}

/* b *= small. Returns false on overflow (would need more limbs). */
static bool bn_mul_small(Bignum *b, uint32_t small) {
    uint64_t carry = 0;
    for (uint32_t i = 0; i < b->n; i++) {
        uint64_t prod = (uint64_t)b->v[i] * small + carry;
        b->v[i] = (uint32_t)(prod & 0xFFFFFFFFu);
        carry = prod >> 32;
    }
    while (carry) {
        if (b->n >= BN_LIMBS) return false;
        b->v[b->n++] = (uint32_t)(carry & 0xFFFFFFFFu);
        carry >>= 32;
    }
    return true;
}

/* b <<= bits. Returns false on overflow. */
static bool bn_shl(Bignum *b, uint32_t bits) {
    uint32_t words = bits / 32;
    uint32_t shift = bits % 32;
    if (b->n == 0) return true;
    uint32_t need = b->n + words + (shift ? 1u : 0u);
    if (need > BN_LIMBS) return false;
    if (words) {
        for (uint32_t i = b->n; i-- > 0;) b->v[i + words] = b->v[i];
        for (uint32_t i = 0; i < words; i++) b->v[i] = 0;
        b->n += words;
    }
    if (shift) {
        uint32_t carry = 0;
        for (uint32_t i = words; i < b->n; i++) {
            uint32_t cur = b->v[i];
            b->v[i] = (cur << shift) | carry;
            carry = cur >> (32 - shift);
        }
        if (carry) {
            if (b->n >= BN_LIMBS) return false;
            b->v[b->n++] = carry;
        }
    }
    bn_trim(b);
    return true;
}

/* b /= small, returns remainder. Classic school division over limbs. */
static uint32_t bn_div_small(Bignum *b, uint32_t small) {
    uint64_t rem = 0;
    for (uint32_t i = b->n; i-- > 0;) {
        uint64_t cur = (rem << 32) | b->v[i];
        b->v[i] = (uint32_t)(cur / small);
        rem = cur % small;
    }
    bn_trim(b);
    return (uint32_t)rem;
}

/* exact decimal digit generation from a double */

/* Produces up to 18 correct decimal digits (most significant first) and the
 * decimal point position: value = 0.d0d1d2... * 10^point  with d0 != 0
 * (point placement matches the "digits before dot" convention:
 *  123.45 -> digits "12345", point = 3).
 *
 * Method: decompose d = m * 2^e exactly (53-bit m). For e >= 0 the value is
 * the integer m * 2^e -> exact digit string. For e < 0, 2^-n = 5^n * 10^-n
 * exactly, so m * 2^e = (m * 5^-e) * 10^e: generate the digits of the exact
 * integer m * 5^-e and shift the decimal point by e. */

#define DTOA_MAX_DIGITS 18

typedef struct {
    char     digits[DTOA_MAX_DIGITS + 2];
    uint32_t ndigits;      /* digits available (may be fewer than requested) */
    int32_t  point;        /* decimal exponent: digits[0] has weight 10^(point-1) */
    bool     negative;
    bool     truncated;     /* more nonzero digits exist past ndigits         */
} DtoaDigits;

static bool dtoa_digits(double val, DtoaDigits *out, uint32_t want) {
    uint64_t bits = f64_to_bits(val);
    out->negative = (bits & F64_SIGN_BIT) != 0;
    out->truncated = false;

    uint64_t raw_mant = bits & F64_MANT_MASK;
    int32_t  raw_exp  = (int32_t)((bits & F64_EXP_MASK) >> F64_EXP_SHIFT);

    uint64_t m;
    int32_t  e;
    if (raw_exp == 0) {
        /* subnormal or zero */
        if (raw_mant == 0) {
            out->digits[0] = '0';
            out->ndigits = 1;
            out->point = 1;
            return true;
        }
        m = raw_mant;
        e = 1 - F64_BIAS - 52;
    } else {
        m = raw_mant | (1ull << 52);   /* implicit leading 1 */
        e = raw_exp - F64_BIAS - 52;
    }

    if (want > DTOA_MAX_DIGITS) want = DTOA_MAX_DIGITS;

    /* static scratch: the kernel runs one script at a time and these two
     * buffers (1.8 KB) would otherwise sit on a small kernel stack. Not
     * reentrant by design - same contract as the rest of the VM. */
    static Bignum b;
    static char rev[1300];
    int32_t point;

    if (e >= 0) {
        bn_set_u64(&b, m);
        if (!bn_shl(&b, (uint32_t)e)) return false;
        point = 0;   /* pure integer; adjusted below after digit count known */
        /* generate all digits of the integer, MSB-first via /10 loop:
         * digits come out reversed, so collect then flip. */
        rev[0] = '\0';
        uint32_t rn = 0;
        while (!bn_is_zero(&b)) {
            uint32_t r = bn_div_small(&b, 10);
            if (rn < sizeof(rev)) rev[rn++] = (char)('0' + r);
        }
        if (rn == 0) {
            out->digits[0] = '0';
            out->ndigits = 1;
            out->point = 1;
            return true;
        }
        point = (int32_t)rn;   /* integer with rn digits -> point after all */
        uint32_t keep = rn < want ? rn : want;
        uint32_t i;
        for (i = 0; i < keep; i++) out->digits[i] = rev[rn - 1 - i];
        /* rounding: check the digit right after keep for round-half-up */
        bool round_up = false;
        if (keep < rn) {
            out->truncated = true;
            char next = rev[rn - 1 - keep];
            if (next > '5') {
                round_up = true;
            } else if (next == '5') {
                /* any further nonzero digit tips it over half */
                round_up = false;
                for (uint32_t j = keep + 1; j < rn; j++) {
                    if (rev[j] != '0') { round_up = true; break; }
                }
                if (!round_up) round_up = (out->digits[keep - 1] - '0') & 1; /* half-even */
            }
        }
        out->ndigits = keep;
        if (round_up) {
            uint32_t k = keep;
            while (k > 0) {
                k--;
                if (out->digits[k] == '9') { out->digits[k] = '0'; }
                else { out->digits[k]++; break; }
            }
            if (k == 0 && out->digits[0] == '0') {
                /* 999..9 rounded to 1000..0: shift point, keep leading 1 */
                for (uint32_t i2 = 0; i2 + 1 < keep; i2++) out->digits[i2] = out->digits[i2 + 1];
                out->digits[0] = '1';
                point++;
            }
        }
        out->point = point;
        return true;
    } else {
        /* e < 0: value = m * 5^(-e) * 10^e */
        uint32_t n5 = (uint32_t)(-(int64_t)e);
        bn_set_u64(&b, m);
        for (uint32_t i = 0; i < n5; i++) {
            if (!bn_mul_small(&b, 5)) return false;   /* unreachable for valid doubles */
        }
        rev[0] = '\0';
        uint32_t rn = 0;
        while (!bn_is_zero(&b)) {
            uint32_t r = bn_div_small(&b, 10);
            if (rn < sizeof(rev)) rev[rn++] = (char)('0' + r);
        }
        if (rn == 0) {
            out->digits[0] = '0';
            out->ndigits = 1;
            out->point = 1;
            return true;
        }
        /* value = (rn-digit integer) * 10^e  -> point = rn + e */
        point = (int32_t)rn + e;
        /* normalize: strip leading zeros (integer may start with zero digits
         * because 5^n products are small relative to 10^k shift) */
        uint32_t lead = 0;
        while (lead + 1 < rn && rev[rn - 1 - lead] == '0') { lead++; point--; }
        uint32_t avail = rn - lead;
        uint32_t keep = avail < want ? avail : want;
        uint32_t i;
        for (i = 0; i < keep; i++) out->digits[i] = rev[rn - 1 - lead - i];
        bool round_up = false;
        if (keep < avail) {
            out->truncated = true;
            char next = rev[rn - 1 - lead - keep];
            if (next > '5') {
                round_up = true;
            } else if (next == '5') {
                round_up = false;
                for (uint32_t j = keep + 1; j < avail; j++) {
                    if (rev[rn - 1 - lead - j] != '0') { round_up = true; break; }
                }
                if (!round_up) round_up = (out->digits[keep - 1] - '0') & 1;
            }
        }
        out->ndigits = keep;
        if (round_up) {
            uint32_t k = keep;
            while (k > 0) {
                k--;
                if (out->digits[k] == '9') { out->digits[k] = '0'; }
                else { out->digits[k]++; break; }
            }
            if (k == 0 && out->digits[0] == '0') {
                for (uint32_t i2 = 0; i2 + 1 < keep; i2++) out->digits[i2] = out->digits[i2 + 1];
                out->digits[0] = '1';
                point++;
            }
        }
        /* leading-zero normalization above cannot produce point <= 0 for a
         * nonzero normalized value except when digits begin with '0' after
         * rounding cascade; defend against it explicitly. */
        if (out->ndigits > 1 && out->digits[0] == '0') {
            uint32_t shift = 0;
            while (shift < out->ndigits && out->digits[shift] == '0') shift++;
            for (uint32_t i3 = 0; i3 + shift < out->ndigits; i3++)
                out->digits[i3] = out->digits[i3 + shift];
            out->ndigits -= shift;
            point -= (int32_t)shift;
        }
        out->point = point;
        return true;
    }
}

/* %f style: fixed decimal, prec digits after the point */
static uint32_t v_fmt_f64_fixed(char *buf, size_t cap, double val, uint32_t prec) {
    if (cap == 0) return 0;
    if (prec > 400) prec = 400;   /* bound: user format strings cannot blow the stack */
    if (m_isnan(val)) { v_strlcpy(buf, "nan", cap); return 3; }
    if (m_isinf(val)) { v_strlcpy(buf, val < 0 ? "-inf" : "inf", cap); return (uint32_t)v_strlen(buf); }

    DtoaDigits d;
    if (!dtoa_digits(val, &d, DTOA_MAX_DIGITS)) { v_strlcpy(buf, "?", cap); return 1; }

    char out[800];
    uint32_t on = 0;
    if (d.negative) out[on++] = '-';

    int32_t point = d.point;
    if (point <= 0) {
        out[on++] = '0';
        if (prec) {
            out[on++] = '.';
            for (uint32_t i = 0; i < prec; i++) {
                int32_t didx = point + (int32_t)i;
                out[on++] = (didx >= 0 && (uint32_t)didx < d.ndigits) ? d.digits[didx] : '0';
            }
        }
    } else {
        uint32_t ipart = (uint32_t)point;
        for (uint32_t i = 0; i < ipart; i++) {
            out[on++] = (i < d.ndigits) ? d.digits[i] : '0';
        }
        if (prec) {
            out[on++] = '.';
            for (uint32_t i = 0; i < prec; i++) {
                uint32_t didx = ipart + i;
                out[on++] = (didx < d.ndigits) ? d.digits[didx] : '0';
            }
        }
    }
    out[on] = '\0';

    /* rounding beyond 18 significant digits: trailing digits were zero
     * extended, and dtoa_digits already rounded the 18th, so the fixed
     * string is as exact as a double can express. */
    if (on + 1 >= cap) on = (uint32_t)cap - 1;
    v_memcpy(buf, out, on);
    buf[on] = '\0';
    return on;
}

/* %e style: d.ddde+XX */
static uint32_t v_fmt_f64_exp(char *buf, size_t cap, double val, uint32_t prec, bool upper) {
    if (cap == 0) return 0;
    if (prec > 40) prec = 40;
    if (m_isnan(val)) { v_strlcpy(buf, upper ? "NAN" : "nan", cap); return 3; }
    if (m_isinf(val)) {
        v_strlcpy(buf, val < 0 ? (upper ? "-INF" : "-inf") : (upper ? "INF" : "inf"), cap);
        return (uint32_t)v_strlen(buf);
    }

    DtoaDigits d;
    if (!dtoa_digits(val, &d, DTOA_MAX_DIGITS)) { v_strlcpy(buf, "?", cap); return 1; }

    char out[64];
    uint32_t on = 0;
    if (d.negative) out[on++] = '-';
    out[on++] = (d.ndigits > 0) ? d.digits[0] : '0';
    if (prec) {
        out[on++] = '.';
        for (uint32_t i = 0; i < prec; i++) {
            out[on++] = (i + 1 < d.ndigits) ? d.digits[i + 1] : '0';
        }
    }
    out[on++] = upper ? 'E' : 'e';
    int32_t ex = d.point - 1;
    out[on++] = ex < 0 ? '-' : '+';
    uint32_t exu = ex < 0 ? (uint32_t)(-ex) : (uint32_t)ex;
    if (exu >= 100) out[on++] = (char)('0' + exu / 100);
    out[on++] = (char)('0' + (exu / 10) % 10);
    out[on++] = (char)('0' + exu % 10);
    out[on] = '\0';

    uint32_t w = on < cap - 1 ? on : (uint32_t)cap - 1;
    v_memcpy(buf, out, w);
    buf[w] = '\0';
    return w;
}

/* %g style: prec = significant digits; strips trailing zeros; switches to
 * exponential when point < -4 or point > prec. This is what str(float) and
 * println use: shortest faithful representation. */
static uint32_t v_fmt_f64_g(char *buf, size_t cap, double val, uint32_t prec, bool upper) {
    if (cap == 0) return 0;
    if (m_isnan(val)) { v_strlcpy(buf, upper ? "NAN" : "nan", cap); return 3; }
    if (m_isinf(val)) {
        v_strlcpy(buf, val < 0 ? (upper ? "-INF" : "-inf") : (upper ? "INF" : "inf"), cap);
        return (uint32_t)v_strlen(buf);
    }
    if (prec == 0) prec = 1;

    DtoaDigits d;
    if (!dtoa_digits(val, &d, prec < DTOA_MAX_DIGITS ? prec : DTOA_MAX_DIGITS)) {
        v_strlcpy(buf, "?", cap);
        return 1;
    }

    /* integer-valued doubles print without any decimal part */
    bool is_int_like = (d.point >= 1 && (uint32_t)d.point >= d.ndigits);

    if (d.point < -4 || (uint32_t)d.point > prec) {
        char tmp[64];
        v_fmt_f64_exp(tmp, sizeof(tmp), val,
                      prec > d.ndigits ? d.ndigits - 1 : prec - 1, upper);
        /* strip trailing zeros in the mantissa like %g does */
        char out[64];
        uint32_t on = 0;
        char *dot = NULL;
        char *epos = NULL;
        for (uint32_t i = 0; tmp[i]; i++) {
            if (tmp[i] == '.') dot = &out[on];
            if (tmp[i] == 'e' || tmp[i] == 'E') { epos = &tmp[i]; break; }
            out[on++] = tmp[i];
        }
        if (dot && epos) {
            uint32_t cut = (uint32_t)(on);
            while (cut > 0 && out[cut - 1] == '0') cut--;
            if (cut > 0 && out[cut - 1] == '.') cut--;
            on = cut;
            for (uint32_t i = 0; epos[i]; i++) out[on++] = epos[i];
        }
        out[on] = '\0';
        v_strlcpy(buf, out, cap);
        return on;
    }

    char out[800];
    uint32_t on = 0;
    if (d.negative) out[on++] = '-';
    if (d.point <= 0) {
        out[on++] = '0';
        uint32_t fz = (uint32_t)(-d.point);
        for (uint32_t i = 0; i < fz; i++) out[on++] = '0';
        for (uint32_t i = 0; i < d.ndigits; i++) out[on++] = d.digits[i];
    } else {
        uint32_t ip = (uint32_t)d.point;
        for (uint32_t i = 0; i < ip; i++) out[on++] = (i < d.ndigits) ? d.digits[i] : '0';
        if (!is_int_like && d.ndigits > ip) {
            out[on++] = '.';
            for (uint32_t i = ip; i < d.ndigits; i++) out[on++] = d.digits[i];
        }
    }
    out[on] = '\0';
    v_strlcpy(buf, out, cap);
    return (uint32_t)v_strlen(buf);
}

/* parsing */

static int64_t v_parse_i64(const char *s, uint32_t len, uint32_t base, bool *ok) {
    *ok = false;
    if (len == 0) return 0;
    uint32_t i = 0;
    bool neg = false;
    if (s[i] == '+' || s[i] == '-') { neg = (s[i] == '-'); i++; }
    if (i >= len) return 0;

    uint64_t limit = neg ? 9223372036854775808ull : 9223372036854775807ull;
    uint64_t acc = 0;
    uint64_t cutoff = limit / base;
    uint32_t cutrem = (uint32_t)(limit % base);
    uint32_t ndig = 0;

    for (; i < len; i++) {
        char c = s[i];
        uint32_t dig;
        if (c >= '0' && c <= '9')      dig = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'z') dig = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'Z') dig = (uint32_t)(c - 'A' + 10);
        else return 0;
        if (dig >= base) return 0;
        if (acc > cutoff || (acc == cutoff && dig > cutrem)) return 0;  /* overflow */
        acc = acc * base + dig;
        ndig++;
    }
    if (ndig == 0) return 0;
    *ok = true;
    return neg ? -(int64_t)(acc - 1) - 1 : (int64_t)acc;
}

/* strtod: exact for up to 15 significant digits with |dec exponent| <= 22
 * (the IEEE-correct fast path), single-step scaled path beyond that. Values
 * past the fast path lose at most an ulp or two - documented, and irrelevant
 * for a shell scripting language. */
static double v_parse_f64(const char *s, uint32_t len, bool *ok) {
    *ok = false;
    if (len == 0) return 0.0;
    uint32_t i = 0;
    bool neg = false;
    while (i < len && is_space_char(s[i])) i++;
    if (i < len && (s[i] == '+' || s[i] == '-')) { neg = (s[i] == '-'); i++; }

    if (i + 2 < len && s[i] == 'i' && s[i + 1] == 'n' && s[i + 2] == 'f') {
        *ok = true;
        return make_inf(neg);
    }
    if (i + 2 < len && s[i] == 'n' && s[i + 1] == 'a' && s[i + 2] == 'n') {
        *ok = true;
        return make_nan();
    }

    uint64_t mant = 0;
    uint32_t ndig = 0;
    int32_t  exp10 = 0;
    bool any = false;

    while (i < len && s[i] == '0') { i++; any = true; }        /* leading zeros */
    while (i < len && s[i] >= '0' && s[i] <= '9') {
        if (ndig < 19) { mant = mant * 10 + (uint64_t)(s[i] - '0'); ndig++; }
        else exp10++;                                            /* extra digits scale */
        any = true;
        i++;
    }
    if (i < len && s[i] == '.') {
        i++;
        while (i < len && s[i] >= '0' && s[i] <= '9') {
            if (ndig < 19) { mant = mant * 10 + (uint64_t)(s[i] - '0'); ndig++; exp10--; }
            any = true;
            i++;
        }
    }
    if (!any) return 0.0;

    if (i < len && (s[i] == 'e' || s[i] == 'E')) {
        uint32_t esave = i;
        i++;
        bool eneg = false;
        if (i < len && (s[i] == '+' || s[i] == '-')) { eneg = (s[i] == '-'); i++; }
        uint32_t eacc = 0;
        bool eany = false;
        while (i < len && s[i] >= '0' && s[i] <= '9') {
            if (eacc < 100000) eacc = eacc * 10 + (uint32_t)(s[i] - '0');
            eany = true;
            i++;
        }
        if (!eany) { i = esave; }   /* 'e' not followed by digits: literal ends before e */
        else if (eneg) {
            if ((int64_t)eacc + (int64_t)exp10 < -30000) { *ok = true; return neg ? -0.0 : 0.0; }
            exp10 -= (int32_t)eacc;
        } else {
            if ((int64_t)eacc + (int64_t)exp10 > 30000) { *ok = true; return make_inf(neg); }
            exp10 += (int32_t)eacc;
        }
    }

    /* more than 19 digits: mantissa rounding already biased toward the first
     * 19; recompute exp10 correction applied above keeps magnitude sane. */
    double d = (double)mant;
    int32_t e = exp10;

    if (e >= -22 && e <= 22) {
        if (e >= 0) d *= pow10_exact[e];
        else        d /= pow10_exact[-e];
    } else {
        /* chunked scaling: split e into a fast-path part and 1e22 steps */
        if (e > 0) {
            while (e > 22) { d *= 1e22; e -= 22; if (m_isinf(d)) break; }
            if (e > 0) d *= pow10_exact[e];
        } else {
            while (e < -22) { d *= 1e-22; e += 22; if (d == 0.0) break; }
            if (e < 0) d /= pow10_exact[-e];
        }
    }

    if (m_isinf(d)) { *ok = true; return make_inf(neg); }
    *ok = true;
    return neg ? -d : d;
}

/* P04 - UTF-8 utilities
 * The byte string is the source of truth everywhere in the VM: indexing a
 * string walks codepoints, len() counts codepoints, and the lexer only ever
 * needs ASCII. Decoding is strict - overlong forms, surrogates and >U+10FFFF
 * are rejected, because accepting them would let scripts smuggle invalid
 * UTF-8 into the terminal renderer. */

#define UTF8_INVALID 0xFFFFFFFFu

static uint32_t utf8_seq_len(uint8_t lead) {
    if (lead < 0x80) return 1;
    if ((lead & 0xE0u) == 0xC0u) return 2;
    if ((lead & 0xF0u) == 0xE0u) return 3;
    if ((lead & 0xF8u) == 0xF0u) return 4;
    return 0;   /* continuation byte or invalid lead */
}

/* Decode one codepoint at s[0..]. Returns bytes consumed, 0 on error. */
static uint32_t utf8_decode(const char *s, uint32_t len, uint32_t *cp_out) {
    if (len == 0) { *cp_out = 0; return 0; }
    uint8_t lead = (uint8_t)s[0];
    uint32_t n = utf8_seq_len(lead);
    if (n == 0 || n > len) { *cp_out = UTF8_INVALID; return 0; }

    uint32_t cp;
    switch (n) {
        case 1: cp = lead; break;
        case 2: cp = lead & 0x1Fu; break;
        case 3: cp = lead & 0x0Fu; break;
        default: cp = lead & 0x07u; break;
    }
    for (uint32_t i = 1; i < n; i++) {
        uint8_t c = (uint8_t)s[i];
        if ((c & 0xC0u) != 0x80u) { *cp_out = UTF8_INVALID; return 0; }
        cp = (cp << 6) | (uint32_t)(c & 0x3Fu);
    }
    /* reject overlong encodings, surrogates, out-of-range */
    static const uint32_t min_cp[5] = { 0, 0, 0x80, 0x800, 0x10000 };
    if (cp < min_cp[n]) { *cp_out = UTF8_INVALID; return 0; }
    if (cp >= 0xD800 && cp <= 0xDFFF) { *cp_out = UTF8_INVALID; return 0; }
    if (cp > 0x10FFFF) { *cp_out = UTF8_INVALID; return 0; }
    *cp_out = cp;
    return n;
}

/* Encode one codepoint; returns byte length (0 if cp invalid). */
static uint32_t utf8_encode(uint32_t cp, char out[4]) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        if (cp >= 0xD800 && cp <= 0xDFFF) return 0;   /* no surrogates */
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000) {
        if (cp >= 0xD800 && cp <= 0xDFFF) return 0;
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    if (cp <= 0x10FFFF) {
        out[0] = (char)(0xF0u | (cp >> 18));
        out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
        out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[3] = (char)(0x80u | (cp & 0x3Fu));
        return 4;
    }
    return 0;
}

/* Count codepoints in a validated-or-not byte string. Invalid sequences
 * count as one unit each so len() never disagrees with a full walk. */
static uint32_t utf8_length(const char *s, uint32_t len) {
    uint32_t count = 0;
    uint32_t i = 0;
    while (i < len) {
        uint32_t cp;
        uint32_t n = utf8_decode(s + i, len - i, &cp);
        if (n == 0) { i++; count++; continue; }   /* invalid byte: count and skip */
        i += n;
        count++;
    }
    return count;
}

static bool utf8_validate(const char *s, uint32_t len) {
    uint32_t i = 0;
    while (i < len) {
        uint32_t cp;
        uint32_t n = utf8_decode(s + i, len - i, &cp);
        if (n == 0) return false;
        i += n;
    }
    return true;
}

/* Byte offset of codepoint #idx; returns len if idx is past the end. */
static uint32_t utf8_byte_offset(const char *s, uint32_t len, uint32_t idx) {
    uint32_t i = 0;
    uint32_t seen = 0;
    while (i < len && seen < idx) {
        uint32_t cp;
        uint32_t n = utf8_decode(s + i, len - i, &cp);
        if (n == 0) { i++; seen++; continue; }
        i += n;
        seen++;
    }
    return i;
}

/* Codepoint index of a byte offset (offset must be on a boundary or the
 * result is the count of codepoints starting strictly before it). */
static uint32_t utf8_cp_index(const char *s, uint32_t len, uint32_t byte_off) {
    uint32_t i = 0;
    uint32_t idx = 0;
    while (i < len && i < byte_off) {
        uint32_t cp;
        uint32_t n = utf8_decode(s + i, len - i, &cp);
        if (n == 0) { i++; idx++; continue; }
        i += n;
        idx++;
    }
    return idx;
}

/* P05 - soft math, part 1: classification, rounding, sqrt/cbrt
 * No libm exists in the kernel image, so Vanta carries its own. Every
 * routine is written against IEEE-754 bit patterns: NaN/Inf semantics are
 * explicit, not accidental. Accuracy target: <= 1 ulp for sqrt/cbrt, <= 2
 * ulp for the transcendentals in P06 - enough for scripting, documented. */

static bool m_isnan(double x) {
    uint64_t b = f64_to_bits(x);
    return (b & F64_EXP_MASK) == F64_EXP_MASK && (b & F64_MANT_MASK) != 0;
}

static bool m_isinf(double x) {
    uint64_t b = f64_to_bits(x);
    return (b & F64_EXP_MASK) == F64_EXP_MASK && (b & F64_MANT_MASK) == 0;
}

static bool m_finite(double x) {
    return (f64_to_bits(x) & F64_EXP_MASK) != F64_EXP_MASK;
}

static bool m_signbit(double x) {
    return (f64_to_bits(x) & F64_SIGN_BIT) != 0;
}

static double m_copysign(double x, double s) {
    uint64_t xb = f64_to_bits(x);
    uint64_t sb = f64_to_bits(s);
    return bits_to_f64((xb & ~F64_SIGN_BIT) | (sb & F64_SIGN_BIT));
}

static double m_fabs(double x) {
    return bits_to_f64(f64_to_bits(x) & ~F64_SIGN_BIT);
}

static double m_fmin(double a, double b) {
    if (m_isnan(a)) return b;
    if (m_isnan(b)) return a;
    return a < b ? a : b;
}

static double m_fmax(double a, double b) {
    if (m_isnan(a)) return b;
    if (m_isnan(b)) return a;
    return a > b ? a : b;
}

/* frexp / ldexp: exact exponent manipulation */

/* Split x into mantissa m in [0.5, 1) and exponent e:  x = m * 2^e. */
static double m_frexp(double x, int *exp) {
    uint64_t b = f64_to_bits(x);
    if (x == 0.0 || (b & F64_EXP_MASK) == F64_EXP_MASK) {   /* zero, nan, inf */
        *exp = 0;
        return x;
    }
    if ((b & F64_EXP_MASK) != 0) {
        /* normal: same mantissa bits, exponent forced to -1 -> [0.5, 1) */
        double norm = bits_to_f64((b & ~F64_EXP_MASK) |
                                  ((uint64_t)(F64_BIAS - 1) << F64_EXP_SHIFT));
        *exp = (int)((b & F64_EXP_MASK) >> F64_EXP_SHIFT) - F64_BIAS + 1;
        return norm;
    }
    /* subnormal: shift the mantissa left until the implicit-bit position is
     * set. x = mant_raw * 2^-1074; with mant_norm = mant_raw << sh we have
     * x = (mant_norm * 2^-53) * 2^(53 - sh - 1074) and mant_norm*2^-53 lies
     * in [0.5, 1). Rebuild that mantissa with biased exponent BIAS-1. */
    uint64_t mant = b & F64_MANT_MASK;
    int32_t sh = 0;
    while ((mant & (1ull << 52)) == 0) { mant <<= 1; sh++; }
    double norm = bits_to_f64((b & F64_SIGN_BIT) |
                              ((uint64_t)(F64_BIAS - 1) << F64_EXP_SHIFT) |
                              (mant & F64_MANT_MASK));
    *exp = 53 - sh - 1074;
    return norm;
}

static double m_ldexp(double x, int exp) {
    if (x == 0.0 || !m_finite(x)) return x;
    double result = x;
    int rem = exp;
    while (rem != 0) {
        int step = rem;
        if (step > 1000) step = 1000;
        if (step < -1000) step = -1000;
        /* multiply by exact 2^step; biased exponent step+1023 is normal for
         * |step| <= 1000 */
        result *= bits_to_f64((uint64_t)(step + F64_BIAS) << F64_EXP_SHIFT);
        if (result == 0.0 || m_isinf(result)) return result;   /* pinned by limits */
        rem -= step;
    }
    return result;
}

/* rounding family */

static double m_trunc(double x) {
    if (!m_finite(x)) return x;
    double a = m_fabs(x);
    if (a >= 4503599627370496.0) return x;   /* 2^52: already integral */
    double t = (double)(int64_t)a;           /* truncating cast is exact here */
    return m_copysign(t, x);
}

static double m_floor(double x) {
    if (!m_finite(x)) return x;
    double t = m_trunc(x);
    if (t > x) t -= 1.0;
    return t;
}

static double m_ceil(double x) {
    if (!m_finite(x)) return x;
    double t = m_trunc(x);
    if (t < x) t += 1.0;
    return t;
}

static double m_round(double x) {
    /* round half away from zero (math-class rounding, not banker's) */
    if (!m_finite(x)) return x;
    double a = m_fabs(x);
    double t = m_floor(a);
    if (a - t >= 0.5) t += 1.0;
    return m_copysign(t, x);
}

static double m_fmod(double x, double y) {
    if (m_isnan(x) || m_isnan(y)) return make_nan();
    if (m_isinf(x)) return make_nan();       /* fmod(inf, y) = NaN */
    if (y == 0.0)   return make_nan();
    if (x == 0.0)   return x;
    if (!m_finite(y)) return x;

    double q = m_trunc(x / y);
    double r = x - q * y;
    /* quotient rounding can leave |r| >= |y|: one correction step */
    if (m_fabs(r) >= m_fabs(y)) r -= (r > 0.0 ? y : -y);
    if (r == 0.0) r = m_copysign(0.0, x);
    return r;
}

/* sqrt: bit-trick seed + Newton to the last ulp */

static double m_sqrt(double x) {
    if (m_isnan(x)) return x;
    if (m_signbit(x)) {
        if (x == 0.0) return x;              /* sqrt(-0) = -0 */
        return make_nan();
    }
    if (x == 0.0 || m_isinf(x)) return x;

    double a = x;
    bool scaled = false;
    if (a < 2.2250738585072014e-308) {       /* subnormal: rescale into range */
        a *= 4503599627370496.0;             /* 2^52 */
        scaled = true;
    }

    /* seed: halve the exponent in bit space; the magic constant corrects the
     * mantissa estimate to within ~8% */
    uint64_t b = f64_to_bits(a);
    b = (b >> 1) + 0x1FF8000000000000ull;
    double y = bits_to_f64(b);

    /* five Newton rounds y <- (y + a/y)/2 halve the error each time:
     * 8% -> ~0.3% -> 5e-5 -> 1e-9 -> 4e-18 -> exact within 1 ulp. */
    y = 0.5 * (y + a / y);
    y = 0.5 * (y + a / y);
    y = 0.5 * (y + a / y);
    y = 0.5 * (y + a / y);
    y = 0.5 * (y + a / y);

    /* last-ulp selection between y and its neighbor below */
    double y_lo = bits_to_f64(f64_to_bits(y) - 1);
    if (m_fabs(y_lo * y_lo - a) < m_fabs(y * y - a)) y = y_lo;

    if (scaled) y *= 6.7108864e-8;           /* 2^-26: undo half of 2^52 */
    return y;
}

/* cbrt: exponent/3 bit seed + Newton */

static double m_cbrt(double x) {
    if (m_isnan(x)) return x;
    if (x == 0.0 || m_isinf(x)) return x;

    double a = m_fabs(x);
    bool scaled = false;
    if (a < 2.2250738585072014e-308) {       /* subnormal */
        a *= 18014398509481984.0;            /* 2^54: divisible by 3 */
        scaled = true;
    }

    /* seed y ~= a^(1/3): log2(y) = log2(a)/3 in bit space.
     * bits(y) = bits(a)/3 + (2/3)*B, B = 1023<<52 -> 0x2AA0000000000000.
     * Integer division of the whole 64-bit pattern approximates the mantissa
     * cube root crudely (~20% off) which Newton then refines quadratically. */
    uint64_t b = f64_to_bits(a) / 3ull + 0x2AA0000000000000ull;
    double y = bits_to_f64(b);
    if (y == 0.0 || !m_finite(y)) y = a;

    /* Newton for y^3 = a:  y <- (2y + a/y^2)/3, six rounds */
    for (int i = 0; i < 6; i++) {
        y = (2.0 * y + a / (y * y)) / 3.0;
    }

    if (scaled) y *= 3.814697265625e-7;      /* 2^-18: cbrt(2^-54) */
    return m_copysign(y, x);
}

/* hypot: scaled to dodge overflow/underflow */

static double m_hypot(double x, double y) {
    if (m_isinf(x) || m_isinf(y)) return make_inf(false);   /* inf dominates NaN */
    if (m_isnan(x) || m_isnan(y)) return make_nan();
    x = m_fabs(x);
    y = m_fabs(y);
    if (x < y) { double t = x; x = y; y = t; }
    if (x == 0.0) return 0.0;
    double r = y / x;
    return x * m_sqrt(1.0 + r * r);
}

/* P06 - soft math, part 2: exp/log/pow and the trigonometric family
 * Coefficients are fdlibm-grade minimax polynomials. Argument reduction for
 * sin/cos uses a multi-term Cody-Waite split of pi/2, exact for |x| < 2^25;
 * beyond that accuracy degrades gracefully - documented, no Payne-Hanek
 * table for a shell scripting language. All entry points are total: every
 * NaN/Inf/zero case has an explicit branch. */

/* exp */

static double m_expm1(double x);   /* forward: used by tanh */

static double m_exp(double x) {
    static const double
        LN2_HI  = 6.93147180369123816490e-01,
        LN2_LO  = 1.90821492927058770002e-10,
        INV_LN2 = 1.44269504088896338700e+00;

    if (m_isnan(x)) return x;
    if (x == 0.0)   return 1.0;
    if (x >  709.7827128933840868)  return make_inf(false);     /* overflow  */
    if (x < -745.13321910194110842) return 0.0;                 /* underflow */

    /* |x| below half an ulp of 1: 1 + x rounds to the exact neighbor */
    if (m_fabs(x) < 1.1102230246251565e-16) return 1.0 + x;

    /* reduce: x = k*ln2 + r with |r| <= ln2/2 */
    double t = x * INV_LN2 + (x >= 0.0 ? 0.5 : -0.5);
    int k = (int)(int64_t)t;
    double r = x - (double)k * LN2_HI - (double)k * LN2_LO;

    /* e^r via Taylor to r^14; |r| <= 0.347 so the first dropped term is
     * |r|^15/15! ~ 5e-21 - far below 1 ulp of the result */

    // what.
    double p = 1.1470745597729724714e-11;
    p = 1.6059043836821614599e-10 + r * p;
    p = 2.0876756987868098979e-09 + r * p;
    p = 2.5052108385441718775e-08 + r * p;
    p = 2.7557319223985890653e-07 + r * p;
    p = 2.7557319223985890653e-06 + r * p;
    p = 2.4801587301587301587e-05 + r * p;
    p = 1.9841269841269841270e-04 + r * p;
    p = 1.3888888888888888889e-03 + r * p;
    p = 8.3333333333333333333e-03 + r * p;
    p = 4.1666666666666666667e-02 + r * p;
    p = 1.6666666666666666667e-01 + r * p;
    p = 0.5 + r * p;
    p = 1.0 + r * p;
    double e = 1.0 + r * p;

    return m_ldexp(e, k);
}

static double m_expm1(double x) {
    /* accurate e^x - 1 for small |x|, where the naive form cancels away
     * every significant digit */
    if (m_isnan(x)) return x;
    if (x == 0.0)   return x;
    if (x >  709.0) return make_inf(false);
    if (x < -709.0) return -1.0;
    if (m_fabs(x) < 1e-8) return x + 0.5 * x * x;   /* next term: x^3/6 ~ 1.7e-25 */
    return m_exp(x) - 1.0;
}

/* log */

static double m_log(double x) {
    static const double
        LN2_HI = 6.93147180369123816490e-01,
        LN2_LO = 1.90821492927058770002e-10;

    if (m_isnan(x)) return x;
    if (x < 0.0)    return make_nan();
    if (x == 0.0)   return make_inf(true);
    if (m_isinf(x)) return x;

    int e;
    double m = m_frexp(x, &e);            /* m in [0.5, 1) */
    if (m < 0.70710678118654752440) {     /* fold into [sqrt2/2, sqrt2) */
        m *= 2.0;
        e -= 1;
    }

    double f = m - 1.0;                   /* |f| <= sqrt(2)-1 */
    double s = f / (2.0 + f);             /* |s| <= 0.1716 */
    double z = s * s;

    /* log(1+f) = 2s(1 + z/3 + z^2/5 + ... + z^9/19).
     * First dropped term: 2s*z^10/21 <= 0.34*0.0295^10/21 ~ 7e-17. */
    double poly = 0.05263157894736842105;
    poly = 0.05882352941176470588 + z * poly;
    poly = 0.06666666666666666667 + z * poly;
    poly = 0.07692307692307692308 + z * poly;
    poly = 0.09090909090909090909 + z * poly;
    poly = 0.11111111111111111111 + z * poly;
    poly = 0.14285714285714285714 + z * poly;
    poly = 0.2 + z * poly;
    poly = 0.33333333333333333333 + z * poly;
    double series = 1.0 + z * poly;
    double logm = 2.0 * s * series;

    /* hi term last: e*LN2_HI dominates logm in magnitude for extreme scales */
    return (double)e * LN2_HI + (logm + (double)e * LN2_LO);
}

static double m_log10(double x) {
    static const double LOG10_E = 0.43429448190325182765;   /* 1/ln(10) */
    static const double LOG10_2 = 0.30102999566398119521;   /* log10(2) */
    if (m_isnan(x) || x < 0.0) return make_nan();
    if (x == 0.0)  return make_inf(true);
    if (m_isinf(x)) return x;
    int e;
    double m = m_frexp(x, &e);
    return m_log(m) * LOG10_E + (double)e * LOG10_2;
}

/* pow */

static double pow_by_squaring(double x, int64_t n) {
    /* binary exponentiation, n >= 0. Exact whenever the result is exactly
     * representable; otherwise bounded by 2*log2(n) ulps. The exp/log path
     * would turn pow(2,10) into 1023.9999999999999 - unacceptable in a
     * systems language - so every integer exponent comes through here. */
    uint64_t u = (uint64_t)n;
    double base = x;
    double acc = 1.0;
    while (u) {
        if (u & 1ull) {
            acc *= base;
            if (acc == 0.0 || m_isinf(acc)) break;   // pinned: nothing to rescue
        }
        u >>= 1;
        if (u) {
            base *= base;
            if (m_isinf(base)) break;
        }
    }
    return acc;
}

static double m_pow(double x, double y) {
    if (m_isnan(x) || m_isnan(y)) return make_nan();
    if (y == 0.0) return 1.0;              /* pow(anything, 0) = 1, NaN^0 included */
    if (y == 1.0) return x;
    if (x == 1.0) return 1.0;              /* 1^anything = 1, 1^inf included */

    if (m_isinf(y)) {
        double ax = m_fabs(x);
        if (ax == 1.0) return 1.0;
        bool grows = (ax > 1.0) == (y > 0.0);   /* |x|>1 ^ +inf  or  |x|<1 ^ -inf */
        return grows ? make_inf(false) : 0.0;
    }
    if (m_isinf(x)) {
        bool odd = false;
        if (x < 0.0) {
            double ay = m_fabs(y);
            // odd integer exponent flips the sign
            odd = (ay == m_trunc(ay)) && (ay < 9007199254740992.0) &&
                  (((int64_t)ay) & 1) != 0;
        }
        if (y > 0.0) return (x > 0.0 || !odd) ? make_inf(false) : make_inf(true);
        return (x > 0.0 || !odd) ? 0.0 : bits_to_f64(F64_SIGN_BIT);   /* -0 */
    }

    if (y == 2.0)  return x * x;
    if (y == 0.5)  return m_sqrt(x);
    if (y == -0.5) return 1.0 / m_sqrt(x);
    if (y == -1.0) return 1.0 / x;

    bool y_is_int = (y == m_trunc(y)) && (m_fabs(y) < 2147483647.0);

    if (x == 0.0) {
        if (y > 0.0) {
            bool odd_int = y_is_int && (((int64_t)y) & 1) != 0;
            return (m_signbit(x) && odd_int) ? bits_to_f64(F64_SIGN_BIT) : 0.0;
        }
        bool odd_int = y_is_int && (((int64_t)(-y)) & 1) != 0;
        return make_inf(m_signbit(x) && odd_int);
    }

    if (y_is_int) {
        int64_t n = (int64_t)y;
        if (x > 0.0) return n < 0 ? 1.0 / pow_by_squaring(x, -n)
                                  : pow_by_squaring(x, n);
        /* negative base, integer exponent: magnitude path + parity sign */
        int64_t m = n < 0 ? -n : n;
        double r = pow_by_squaring(-x, m);
        if (m & 1) r = -r;
        return n < 0 ? 1.0 / r : r;
    }

    if (x < 0.0) return make_nan();        /* fractional power of negative */

    /* general positive path: x^y = exp(y*ln x) */
    double r = y * m_log(x);
    if (r >  709.0)  return make_inf(false);
    if (r < -745.0)  return 0.0;
    return m_exp(r);
}

/* trigonometric kernels */

/* sin for |r| <= pi/4 */
static double k_sin(double r) {
    double z = r * r;
    double p = -1.66666666666666324348e-01 + z * (
                8.33333333332248946124e-03 + z * (
               -1.98412698298579493134e-04 + z * (
                2.75573137070700676789e-06 + z * (
               -2.50507602534068634195e-08 + z * (
                1.58969099521155010221e-09)))));
    return r + r * z * p;
}

/* cos for |r| <= pi/4: cos(r) = (1 - z/2) + correction + z^2 * P(z), z=r^2.
 * The ((1-w)-hz) term recovers the rounding error of w = 1 - z/2 for small z
 * exactly the way fdlibm does. */
static double k_cos(double r) {
    double z = r * r;
    double p = 4.16666666666666019037e-02 + z * (
              -1.38888888888741095749e-03 + z * (
               2.48015872894767294178e-05 + z * (
              -2.75573143513906633035e-07 + z * (
               2.08757232129817482790e-09 + z * (
              -1.13596475577881948265e-11)))));
    double hz = 0.5 * z;
    double w  = 1.0 - hz;
    return w + (((1.0 - w) - hz) + z * z * p);
}

/* tan for |r| <= pi/4 */
static double k_tan(double r) {
    return m_sin(r) / m_cos(r);
}

/* Reduce x modulo pi/2 with a multi-term Cody-Waite split. Returns quadrant
 * q = n mod 4 and sets *r to the signed remainder, |r| <= pi/4. The high term
 * PIO2_1 keeps 33 mantissa bits, so n*PIO2_1 is exact for |n| < 2^20; the
 * following terms recover the rest. |x| < 2^25 stays within stated accuracy,
 * larger arguments still return finite, correctly-signed results. */
static int reduce_trig(double x, double *r) {
    static const double
        INV_PIO2 = 6.36619772367581382433e-01,   /* 2/pi                */
        PIO2_1   = 1.57079632673412561417e+00,   /* pi/2, top 33 bits   */
        PIO2_1T  = 6.07710050650619224932e-11,   /* pi/2 - PIO2_1       */
        PIO2_2   = 6.07710050630396597660e-11,   /* next chunk of pi/2  */
        PIO2_2T  = 2.02226624879595063154e-21,
        PIO2_3   = 2.02226624871116645580e-21,
        PIO2_3T  = 8.47842766036889956997e-32,
        PIO4     = 7.8539816339744830962e-01;

    double n_double = m_round(x * INV_PIO2);
    int64_t n64 = (int64_t)n_double;
    double fn = n_double;

    double rem;
    if (n64 < 20000 && n64 > -20000) {
        rem = ((x - fn * PIO2_1) - fn * PIO2_1T) - fn * PIO2_2 - fn * PIO2_2T;
    } else {
        rem = ((((x - fn * PIO2_1) - fn * PIO2_1T) - fn * PIO2_2) - fn * PIO2_2T)
              - fn * PIO2_3 - fn * PIO2_3T;
    }

    int64_t q = n64 & 3;

    /* defensive fold: keeps kernels inside their proven range even when the
     * reduction above lost the race with extreme magnitudes */
    while (rem > PIO4)  { rem -= 2.0 * PIO4; q = (q + 1) & 3; }
    while (rem < -PIO4) { rem += 2.0 * PIO4; q = (q + 3) & 3; }

    *r = rem;
    return (int)q;
}

static double m_sin(double x) {
    if (m_isnan(x)) return x;
    if (m_isinf(x)) return make_nan();
    if (m_fabs(x) < 7.8539816339744830962e-01) return k_sin(x);   /* |x| < pi/4 */

    double r;
    int q = reduce_trig(x, &r);
    switch (q) {
        case 0:  return  k_sin(r);
        case 1:  return  k_cos(r);
        case 2:  return -k_sin(r);
        default: return -k_cos(r);
    }
}

static double m_cos(double x) {
    if (m_isnan(x)) return x;
    if (m_isinf(x)) return make_nan();
    if (m_fabs(x) < 7.8539816339744830962e-01) return k_cos(x);

    double r;
    int q = reduce_trig(x, &r);
    switch (q) {
        case 0:  return  k_cos(r);
        case 1:  return -k_sin(r);
        case 2:  return -k_cos(r);
        default: return  k_sin(r);
    }
}

static double m_tan(double x) {
    if (m_isnan(x)) return x;
    if (m_isinf(x)) return make_nan();
    if (m_fabs(x) < 7.8539816339744830962e-01) return k_tan(x);

    double r;
    int q = reduce_trig(x, &r);
    double t = k_tan(r);
    /* quadrants 1/3: tan(pi/2 + r) = -cot(r) */
    return (q == 1 || q == 3) ? -1.0 / t : t;
}

/* inverse trig */

/* pi/2 as a helper so every consumer agrees on the same bits */
static double make_const_pi_half(void) {
    return 1.57079632679489655800e+00;
}

static double m_atan(double x) {
    static const double ATANHI[] = {
        4.63647609000806093515e-01,   /* atan(0.5) */
        7.85398163397448278999e-01,   /* atan(1.0) */
        9.82793723247329054082e-01,   /* atan(1.5) */
        1.57079632679489655800e+00    /* atan(inf) */
    };
    static const double ATANLO[] = {
        2.26987774529616870924e-17,
        3.06161699786838301793e-17,
        1.39033110312309984516e-17,
        6.12323399573676603587e-17
    };
    static const double AT[] = {
         3.33333333333329318027e-01,
        -1.99999999998764832476e-01,
         1.42857142725034663711e-01,
        -1.11111104054623557880e-01,
         9.09088713343650656196e-02,
        -7.69187620504482999495e-02,
         6.66107313738753120669e-02,
        -5.83357013379057348645e-02,
         4.97687799461593236017e-02,
        -3.65315727442169155270e-02,
         1.62858201153657823623e-02
    };
    if (m_isnan(x)) return x;
    if (m_isinf(x)) return x > 0.0 ? make_const_pi_half() : -make_const_pi_half();

    double a = m_fabs(x);
    double z, hi, lo;

    if (a < 0.4375) {                    /* small: series directly on x */
        z = a * a;
        double p = AT[0] + z * (AT[1] + z * (AT[2] + z * (AT[3] + z * (
                   AT[4] + z * (AT[5] + z * (AT[6] + z * (AT[7] + z * (
                   AT[8] + z * (AT[9] + z * AT[10])))))))));
        double t = a + a * z * p;
        return x < 0.0 ? -t : t;
    } else if (a < 1.1875) {             /* reduce around atan(1.0) */
        z = (a - 1.0) / (a + 1.0);
        hi = ATANHI[1];
        lo = ATANLO[1];
    } else if (a < 2.4375) {             /* reduce around atan(1.5) */
        z = (a - 1.5) / (1.0 + 1.5 * a);
        hi = ATANHI[2];
        lo = ATANLO[2];
    } else {                             /* reduce around atan(inf) */
        z = -1.0 / a;
        hi = ATANHI[3];
        lo = ATANLO[3];
    }

    double z2 = z * z;
    double p = AT[0] + z2 * (AT[1] + z2 * (AT[2] + z2 * (AT[3] + z2 * (
               AT[4] + z2 * (AT[5] + z2 * (AT[6] + z2 * (AT[7] + z2 * (
               AT[8] + z2 * (AT[9] + z2 * AT[10])))))))));
    double w = z + z * z2 * p;
    /* hi - ((-w) - lo): adds the tiny correction before the big constant,
     * preserving the last bits of atanhi+atanlo */
    double result = hi - ((-w) - lo);
    return x < 0.0 ? -result : result;
}

static double m_atan2(double y, double x) {
    static const double PI    = 3.14159265358979311600e+00;
    static const double PI_LO = 1.22464679914735320717e-16;   /* lo bits of pi */

    if (m_isnan(x) || m_isnan(y)) return make_nan();
    if (y == 0.0) {
        /* (+-0, +-0) and (+-0, x>0) -> +-0; (+-0, x<0) -> +-pi */
        if (x > 0.0 || (x == 0.0 && !m_signbit(x))) return m_copysign(0.0, y);
        return m_copysign(PI + PI_LO, y);
    }
    if (x == 0.0) return m_copysign(make_const_pi_half(), y);
    if (m_isinf(x)) {
        if (m_isinf(y)) {
            double q = PI * 0.25;
            return x > 0.0 ? m_copysign(q, y) : m_copysign(3.0 * q, y);
        }
        return x > 0.0 ? m_copysign(0.0, y) : m_copysign(PI + PI_LO, y);
    }
    if (m_isinf(y)) return m_copysign(make_const_pi_half(), y);

    double a = m_atan(y / x);
    if (x > 0.0) return a;
    return y >= 0.0 ? a + (PI + PI_LO) : a - (PI + PI_LO);
}

static double m_asin(double x) {
    if (m_isnan(x)) return x;
    if (m_fabs(x) > 1.0) return make_nan();
    if (m_fabs(x) == 1.0) return m_copysign(make_const_pi_half(), x);
    /* asin(x) = atan2(x, sqrt(1-x^2)): no cancellation near +-1 */
    return m_atan2(x, m_sqrt(1.0 - x * x));
}

static double m_acos(double x) {
    if (m_isnan(x)) return x;
    if (m_fabs(x) > 1.0) return make_nan();
    /* acos(x) = atan2(sqrt(1-x^2), x): stable at both ends */
    return m_atan2(m_sqrt((1.0 - x) * (1.0 + x)), x);
}

/* hyperbolic */

static double m_sinh(double x) {
    if (m_isnan(x)) return x;
    if (m_isinf(x)) return x;
    double a = m_fabs(x);
    if (a < 1.0) {
        /* Taylor: x(1 + x^2(1/6 + x^2(1/120 + x^2(1/5040 + x^2/362880)))) */
        double z = x * x;
        return x * (1.0 + z * (0.16666666666666665741 + z * (
                0.0083333333333333332177 + z * (
                0.00019841269841269841253 + z * 2.7557319223985892511e-06))));
    }
    if (a > 710.0) return make_inf(m_signbit(x));
    double e = m_exp(a);
    double r = 0.5 * (e - 1.0 / e);
    return m_signbit(x) ? -r : r;
}

static double m_cosh(double x) {
    if (m_isnan(x)) return x;
    if (m_isinf(x)) return make_inf(false);
    double a = m_fabs(x);
    if (a < 1.0) {
        double z = x * x;
        return 1.0 + z * (0.5 + z * (0.041666666666666664354 + z * (
               0.0013888888888888889419 + z * 2.4801587301587301587e-05)));
    }
    if (a > 710.0) return make_inf(false);
    double e = m_exp(a);
    return 0.5 * (e + 1.0 / e);
}

static double m_tanh(double x) {
    if (m_isnan(x)) return x;
    if (m_isinf(x)) return x > 0.0 ? 1.0 : -1.0;
    double a = m_fabs(x);
    if (a > 20.0) return x > 0.0 ? 1.0 : -1.0;   /* saturated to 1 within 1 ulp */
    double u = 2.0 * a;
    double e = (u < 0.7) ? m_expm1(u) : (m_exp(u) - 1.0);
    double t = e / (e + 2.0);                    /* tanh = (e^2a-1)/(e^2a+1) */
    return x < 0.0 ? -t : t;
}

/* P07 - PRNG: splitmix64 seeding + xoroshiro128** generation
 * Deterministic on purpose: scripts stay reproducible across runs and the
 * selftest can assert on rand sequences after a known randSeed. This feeds
 * shuffles, randInt, randChoice - never crypto. A CSPRNG for keys belongs
 * to the kernel's crypto subsystem, not to a scripting language. */

#define RNG_DEFAULT_SEED 0x9E3779B97F4A7C15ull   /* golden ratio constant */

static uint64_t splitmix64_step(uint64_t *state) {
    uint64_t z = (*state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static uint64_t rotl64(uint64_t x, int k) {
    return (x << k) | (x >> (64 - k));
}

static void rng_seed(VantaVM *vm, uint64_t seed) {
    uint64_t s = seed;
    vm->rng_s[0] = splitmix64_step(&s);
    vm->rng_s[1] = splitmix64_step(&s);
    /* xoroshiro degenerates on the all-zero state: nudge it out */
    if (vm->rng_s[0] == 0 && vm->rng_s[1] == 0) vm->rng_s[0] = 1;
    vm->rng_splitmix = seed;
    vm->rng_ready = 1;
}

static uint64_t rng_next_u64(VantaVM *vm) {
    if (!vm->rng_ready) rng_seed(vm, RNG_DEFAULT_SEED);
    const uint64_t s0 = vm->rng_s[0];
    uint64_t s1 = vm->rng_s[1];
    const uint64_t result = rotl64(s0 * 5, 7) * 9;   /* xoroshiro128** scrambler */

    s1 ^= s0;
    vm->rng_s[0] = rotl64(s0, 24) ^ s1 ^ (s1 << 16);
    vm->rng_s[1] = rotl64(s1, 37);
    return result;
}

/* uniform double in [0, 1): top 53 bits of the u64 stream */
static double rng_next_f64(VantaVM *vm) {
    uint64_t r = rng_next_u64(vm) >> 11;
    return (double)r * 1.1102230246251565404236316680908203125e-16; /* 2^-53 */
}

/* high 64 bits of a 64x64 product, built from 32-bit limbs: i386 (-m32)
 * has no __int128, and I refuse to add a libgcc dependency for one helper */
static uint64_t mul64hi(uint64_t a, uint64_t b) {
    uint64_t a_lo = a & 0xFFFFFFFFull, a_hi = a >> 32;
    uint64_t b_lo = b & 0xFFFFFFFFull, b_hi = b >> 32;
    uint64_t ll = a_lo * b_lo;
    uint64_t lh = a_lo * b_hi;
    uint64_t hl = a_hi * b_lo;
    uint64_t hh = a_hi * b_hi;
    uint64_t mid = (ll >> 32) + (lh & 0xFFFFFFFFull) + (hl & 0xFFFFFFFFull);
    return hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
}

/* uniform int64 in [0, bound) without modulo bias: Lemire's multiply-shift.
 * bound == 0 returns 0 instead of trapping - callers validate ranges. */
static uint64_t rng_bounded_u64(VantaVM *vm, uint64_t bound) {
    if (bound == 0) return 0;
    uint64_t x = rng_next_u64(vm);
    uint64_t lo = x * bound;                     /* low 64 of the product */
    if (lo < bound) {
        uint64_t threshold = (0ull - bound) % bound;   /* (2^64 - bound) % bound */
        while (lo < threshold) {
            x = rng_next_u64(vm);
            lo = x * bound;
        }
    }
    return mul64hi(x, bound);
}

/* uniform int64 in [lo, hi] inclusive; lo > hi swaps, matching script intuition */
static int64_t rng_range_i64(VantaVM *vm, int64_t lo, int64_t hi) {
    if (lo > hi) { int64_t t = lo; lo = hi; hi = t; }
    uint64_t span = (uint64_t)(hi - lo);         /* span < 2^63 after any swap */
    if (span == ~(uint64_t)0) return (int64_t)rng_next_u64(vm);
    return lo + (int64_t)rng_bounded_u64(vm, span + 1);
}

/* P08 - fixed-arena size-class allocator and the string pool
 *
 * One allocator core, two arenas: strings get their own so a runaway string
 * program cannot starve array storage (and vice versa). Chunks come from
 * per-size-class freelists first; when a class empties, the arena bump
 * pointer advances. Freed chunks go back to their class freelist - the
 * arena itself never moves backward, but with refcounting the steady-state
 * of any real script reuses the same handful of chunks. When both the
 * freelist and the bump space are gone, allocation fails with a typed error,
 * never with corruption. */

static const uint32_t str_class_sizes[VANTA_STR_CLASSES] = {
    16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 2048, 4096
};

static const uint32_t obj_class_sizes[VANTA_OBJ_CLASSES] = {
    64, 128, 192, 256, 384, 512, 768, 1024,
    1536, 2048, 3072, 4096, 6144, 8192, 12288, 16384
};

/* Smallest class that fits `want` bytes. Returns ncls when nothing fits. */
static uint32_t arena_pick_class(const uint32_t *sizes, uint32_t ncls, uint32_t want) {
    for (uint32_t i = 0; i < ncls; i++) {
        if (sizes[i] >= want) return i;
    }
    return ncls;
}

static void arena_free_head_read(const char *arena, uint32_t off, uint32_t *next) {
    v_memcpy(next, arena + off, 4);
}

static void arena_free_head_write(char *arena, uint32_t off, uint32_t next) {
    v_memcpy(arena + off, &next, 4);
}

static uint32_t arena_alloc(char *arena, uint32_t arena_cap, uint32_t *used,
                            uint32_t *class_free, uint32_t ncls,
                            const uint32_t *sizes, uint32_t want,
                            uint32_t *cls_out) {
    uint32_t cls = arena_pick_class(sizes, ncls, want);
    if (cls >= ncls) return VANTA_NIL_HANDLE;   /* request larger than any class */

    if (class_free[cls] != VANTA_NIL_HANDLE) {
        uint32_t off = class_free[cls];
        uint32_t next;
        arena_free_head_read(arena, off, &next);
        class_free[cls] = next;
        *cls_out = cls;
        return off;
    }

    uint32_t chunk = sizes[cls];
    if (*used > arena_cap || arena_cap - *used < chunk) return VANTA_NIL_HANDLE;
    uint32_t off = *used;
    *used = off + chunk;
    *cls_out = cls;
    return off;
}

static void arena_free_chunk(char *arena, uint32_t off, uint32_t *class_free, uint32_t cls) {
    arena_free_head_write(arena, off, class_free[cls]);
    class_free[cls] = off;
}

/* string pool */

static bool str_pool_init(VantaVM *vm) {
    /* heads: slot 0 is the pinned empty string (permanent refs=1), the rest
     * form the free-head list linked through data_off. */
    for (uint32_t i = 0; i < VANTA_STR_HEADS; i++) {
        vm->str_heads[i].len = 0;
        vm->str_heads[i].refs = 0;
        vm->str_heads[i].hash = 0;
        vm->str_heads[i].size_cls = 0;
        vm->str_heads[i].pad = 0;
        vm->str_heads[i].data_off = (i + 1 < VANTA_STR_HEADS) ? i + 1 : VANTA_NIL_HANDLE;
    }
    vm->str_head_free = 1;

    vm->str_arena_used = 0;
    for (uint32_t i = 0; i < VANTA_STR_CLASSES; i++) vm->str_class_free[i] = VANTA_NIL_HANDLE;

    /* pinned empty string at offset 0 */
    uint32_t cls;
    uint32_t off = arena_alloc(vm->str_arena, VANTA_STR_ARENA, &vm->str_arena_used,
                               vm->str_class_free, VANTA_STR_CLASSES,
                               str_class_sizes, 1, &cls);
    if (off == VANTA_NIL_HANDLE) return false;
    vm->str_arena[off] = '\0';
    vm->str_heads[0].len = 0;
    vm->str_heads[0].refs = 1;                       /* pinned: never freed */
    vm->str_heads[0].hash = 2166136261u;             /* FNV basis of "" */
    vm->str_heads[0].data_off = off;
    vm->str_heads[0].size_cls = cls;

    vm->str_live = 1;

    vm->sb.len = 0;
    vm->sb.pad = 0;
    vm->sb.space[0] = '\0';

    return true;
}

static bool str_head_valid(VantaVM *vm, uint32_t handle) {
    return handle != VANTA_NIL_HANDLE && handle < VANTA_STR_HEADS &&
           vm->str_heads[handle].refs > 0;
}

/* Allocate a refcounted string (refs starts at 1, owned by the caller).
 * Returns VANTA_NIL_HANDLE when the pool or arena is exhausted. */
static uint32_t str_alloc(VantaVM *vm, const char *bytes, uint32_t len) {
    if (!bytes && len) return VANTA_NIL_HANDLE;
    if (len == 0) {
        str_retain(vm, 0);
        return 0;                       /* the pinned empty string */
    }
    if (len > VANTA_STR_MAX) return VANTA_NIL_HANDLE;

    if (vm->str_head_free == VANTA_NIL_HANDLE) return VANTA_NIL_HANDLE;
    uint32_t handle = vm->str_head_free;
    vm->str_head_free = vm->str_heads[handle].data_off;   /* next free head */

    uint32_t cls;
    uint32_t off = arena_alloc(vm->str_arena, VANTA_STR_ARENA, &vm->str_arena_used,
                               vm->str_class_free, VANTA_STR_CLASSES,
                               str_class_sizes, len + 1, &cls);
    if (off == VANTA_NIL_HANDLE) {
        vm->str_heads[handle].data_off = vm->str_head_free;   /* return head */
        vm->str_head_free = handle;
        return VANTA_NIL_HANDLE;
    }

    if (len) v_memcpy(vm->str_arena + off, bytes, len);
    vm->str_arena[off + len] = '\0';    /* NUL kept for c_puts interop */

    VantaStrHead *h = &vm->str_heads[handle];
    h->len = len;
    h->refs = 1;
    h->hash = v_hash_bytes((const uint8_t *)bytes, len);
    h->data_off = off;
    h->size_cls = cls;
    vm->str_live++;
    return handle;
}

static void str_retain(VantaVM *vm, uint32_t handle) {
    if (handle == VANTA_NIL_HANDLE) return;
    if (handle >= VANTA_STR_HEADS) return;
    VantaStrHead *h = &vm->str_heads[handle];
    if (h->refs == 0) return;           /* stale handle: refuse to resurrect */
    if (h->refs < 0xFFFFFFFFu) h->refs++;
}

static void str_release(VantaVM *vm, uint32_t handle) {
    if (handle == VANTA_NIL_HANDLE || handle == 0) return;   /* 0 is pinned */
    if (handle >= VANTA_STR_HEADS) return;
    VantaStrHead *h = &vm->str_heads[handle];
    if (h->refs == 0) return;           /* double free: ignore, don't corrupt */
    h->refs--;
    if (h->refs > 0) return;

    arena_free_chunk(vm->str_arena, h->data_off, vm->str_class_free, h->size_cls);
    h->data_off = vm->str_head_free;
    vm->str_head_free = handle;
    vm->str_live--;
}

static const char *str_data(VantaVM *vm, uint32_t handle) {
    static const char empty[1] = "";
    if (!str_head_valid(vm, handle)) return empty;
    return vm->str_arena + vm->str_heads[handle].data_off;
}

static uint32_t str_len(VantaVM *vm, uint32_t handle) {
    if (!str_head_valid(vm, handle)) return 0;
    return vm->str_heads[handle].len;
}

static uint32_t str_hash_of(VantaVM *vm, uint32_t handle) {
    if (!str_head_valid(vm, handle)) return 0;
    return vm->str_heads[handle].hash;
}

static bool str_equal(VantaVM *vm, uint32_t a, uint32_t b) {
    if (a == b) return true;
    if (!str_head_valid(vm, a) || !str_head_valid(vm, b)) return false;
    VantaStrHead *ha = &vm->str_heads[a];
    VantaStrHead *hb = &vm->str_heads[b];
    if (ha->hash != hb->hash || ha->len != hb->len) return false;
    return v_memcmp_eq(vm->str_arena + ha->data_off, vm->str_arena + hb->data_off, ha->len);
}

/* byte-order comparison for sort and relational operators */
static int32_t str_compare(VantaVM *vm, uint32_t a, uint32_t b) {
    if (a == b) return 0;
    uint32_t la = str_len(vm, a);
    uint32_t lb = str_len(vm, b);
    uint32_t n = la < lb ? la : lb;
    int c = n ? v_memcmp_ord(str_data(vm, a), str_data(vm, b), n) : 0;
    if (c != 0) return c;
    if (la == lb) return 0;
    return la < lb ? -1 : 1;
}

/* substring helper: pooled new string from [start, start+count) clamped */
static uint32_t str_sub(VantaVM *vm, uint32_t handle, uint32_t start, uint32_t count) {
    uint32_t len = str_len(vm, handle);
    if (start >= len) { str_retain(vm, 0); return 0; }
    if (count > len - start) count = len - start;
    return str_alloc(vm, str_data(vm, handle) + start, count);
}

/* string builder
 * The SB is a single scratch buffer inside the VM used for concatenation and
 * formatting. Concat paths never nest (ADD completes before the next ADD),
 * and every builtin that formats gets its own bounded local buffer instead,
 * so one shared SB is safe - and keeps 8 KB off the kernel stack. */

static void sb_reset(VantaSB *sb) {
    sb->len = 0;
    sb->space[0] = '\0';
}

static bool sb_reserve(VantaSB *sb, uint32_t extra) {
    if (extra > VANTA_SB_MAX) return false;
    if (sb->len > VANTA_SB_MAX - 1 - extra) return false;   /* room for NUL */
    return true;
}

static bool sb_append(VantaSB *sb, const char *bytes, uint32_t len) {
    if (!bytes || len == 0) return true;
    if (!sb_reserve(sb, len)) return false;
    v_memcpy(sb->space + sb->len, bytes, len);
    sb->len += len;
    sb->space[sb->len] = '\0';
    return true;
}

static bool sb_append_char(VantaSB *sb, char c) {
    if (!sb_reserve(sb, 1)) return false;
    sb->space[sb->len++] = c;
    sb->space[sb->len] = '\0';
    return true;
}

static bool sb_append_cstr(VantaSB *sb, const char *s) {
    return sb_append(sb, s, (uint32_t)v_strlen(s));
}

/* commit the SB contents into the string pool; resets the SB. Returns a
 * handle with refs=1 owned by the caller, or NIL_HANDLE on OOM. */
static uint32_t str_take_sb(VantaVM *vm, VantaSB *sb) {
    uint32_t h = str_alloc(vm, sb->space, sb->len);
    sb_reset(sb);
    return h;
}

/* vsnprintf straight into the SB (used by format() and error building) */
static bool sb_appendf(VantaSB *sb, const char *fmt, ...) {
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vanta_vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) return false;
    if ((size_t)n < sizeof(tmp)) return sb_append(sb, tmp, (uint32_t)n);
    /* longer than the local buffer: clamp to it - callers size tmp for the
     * message classes they emit */
    return sb_append(sb, tmp, (uint32_t)v_strlen(tmp));
}

/* P09 - object pool: arrays
 *
 * Ownership contract for every function here (the interpreter depends on it):
 *   arr_new / arr_concat / arr_slice  -> return a handle with refs = 1,
 *                                        owned by the CALLER
 *   arr_get     -> copies the element AND retains it; caller must release
 *   arr_push / arr_insert / arr_set   -> RETAIN the incoming value
 *   arr_set additionally releases the displaced element
 *   arr_pop / arr_remove_at           -> TRANSFER the element to *out (no
 *                                        retain); if *out is NULL the value
 *                                        is released here
 * Breaking this contract double-frees or leaks; every call site in this file
 * was reviewed against it.
 *
 * Element storage lives in obj_arena size-class chunks. Growth allocates the
 * next class, copies, and frees the old chunk - capacity always advances in
 * powers of two up to VANTA_ARR_MAX (1024 elements). */

static bool obj_pool_init(VantaVM *vm) {
    for (uint32_t i = 0; i < VANTA_ARR_HEADS; i++) {
        vm->arr_heads[i].refs = 0;
        vm->arr_heads[i].len = 0;
        vm->arr_heads[i].cap = 0;
        vm->arr_heads[i].size_cls = 0;
        vm->arr_heads[i].pad[0] = 0;
        vm->arr_heads[i].pad[1] = 0;
        vm->arr_heads[i].pad[2] = 0;
        vm->arr_heads[i].data_off = (i + 1 < VANTA_ARR_HEADS) ? i + 1 : VANTA_NIL_HANDLE;
    }
    vm->arr_head_free = 0;

    for (uint32_t i = 0; i < VANTA_MAP_HEADS; i++) {
        vm->map_heads[i].refs = 0;
        vm->map_heads[i].count = 0;
        vm->map_heads[i].cap = 0;
        vm->map_heads[i].size_cls = 0;
        vm->map_heads[i].pad[0] = 0;
        vm->map_heads[i].pad[1] = 0;
        vm->map_heads[i].pad[2] = 0;
        vm->map_heads[i].data_off = (i + 1 < VANTA_MAP_HEADS) ? i + 1 : VANTA_NIL_HANDLE;
    }
    vm->map_head_free = 0;

    vm->obj_arena_used = 0;
    for (uint32_t i = 0; i < VANTA_OBJ_CLASSES; i++) vm->obj_class_free[i] = VANTA_NIL_HANDLE;

    vm->arr_live = 0;
    vm->map_live = 0;
    return true;
}

static bool arr_head_valid(VantaVM *vm, uint32_t handle) {
    return handle != VANTA_NIL_HANDLE && handle < VANTA_ARR_HEADS &&
           vm->arr_heads[handle].refs > 0;
}

static VantaValue *arr_elems(VantaVM *vm, uint32_t handle) {
    return (VantaValue *)(vm->obj_arena + vm->arr_heads[handle].data_off);
}

/* round an element count up to a served capacity (powers of two, 8..1024) */
static uint32_t arr_round_cap(uint32_t want) {
    uint32_t c = 8;
    while (c < want && c < VANTA_ARR_MAX) c *= 2;
    return want > VANTA_ARR_MAX ? VANTA_ARR_MAX : c;
}

static uint32_t arr_alloc_chunk(VantaVM *vm, uint32_t cap_elems, uint32_t *cls_out) {
    uint32_t bytes = cap_elems * (uint32_t)sizeof(VantaValue);
    return arena_alloc(vm->obj_arena, VANTA_OBJ_ARENA, &vm->obj_arena_used,
                       vm->obj_class_free, VANTA_OBJ_CLASSES,
                       obj_class_sizes, bytes, cls_out);
}

/* cap_hint counts elements the caller expects to store soon; 0 is legal */
static uint32_t arr_new(VantaVM *vm, uint32_t cap_hint) {
    if (vm->arr_head_free == VANTA_NIL_HANDLE) return VANTA_NIL_HANDLE;

    uint32_t cap = arr_round_cap(cap_hint < 4 ? 4 : cap_hint);
    uint32_t cls;
    uint32_t off = arr_alloc_chunk(vm, cap, &cls);
    if (off == VANTA_NIL_HANDLE) return VANTA_NIL_HANDLE;

    uint32_t handle = vm->arr_head_free;
    vm->arr_head_free = vm->arr_heads[handle].data_off;

    VantaArrHead *h = &vm->arr_heads[handle];
    h->refs = 1;
    h->len = 0;
    h->cap = cap;
    h->data_off = off;
    h->size_cls = cls;
    vm->arr_live++;
    return handle;
}

static void arr_retain(VantaVM *vm, uint32_t handle) {
    if (!arr_head_valid(vm, handle)) return;
    VantaArrHead *h = &vm->arr_heads[handle];
    if (h->refs < 0xFFFFFFFFu) h->refs++;
}

/* Grow element storage to at least min_cap. Old chunk is freed after the
 * element copy; handles to the array itself stay stable. */
static bool arr_grow(VantaVM *vm, uint32_t handle, uint32_t min_cap) {
    VantaArrHead *h = &vm->arr_heads[handle];
    uint32_t want = h->cap * 2;
    if (want < min_cap) want = min_cap;
    want = arr_round_cap(want);
    if (want <= h->cap) return false;          /* already at the served max */

    uint32_t cls;
    uint32_t off = arr_alloc_chunk(vm, want, &cls);
    if (off == VANTA_NIL_HANDLE) return false;

    VantaValue *old = arr_elems(vm, handle);
    VantaValue *neu = (VantaValue *)(vm->obj_arena + off);
    if (h->len) v_memcpy(neu, old, h->len * sizeof(VantaValue));

    arena_free_chunk(vm->obj_arena, h->data_off, vm->obj_class_free, h->size_cls);
    h->data_off = off;
    h->size_cls = cls;
    h->cap = want;
    return true;
}

static void arr_release(VantaVM *vm, uint32_t handle);   /* fwd: defined in P11 value release */

static uint32_t arr_len(VantaVM *vm, uint32_t handle) {
    if (!arr_head_valid(vm, handle)) return 0;
    return vm->arr_heads[handle].len;
}

static const VantaValue *arr_peek(VantaVM *vm, uint32_t handle, uint32_t idx) {
    /* borrowed pointer, no retain: valid only while nothing mutates */
    if (!arr_head_valid(vm, handle)) return NULL;
    if (idx >= vm->arr_heads[handle].len) return NULL;
    return &arr_elems(vm, handle)[idx];
}

static bool arr_get(VantaVM *vm, uint32_t handle, uint32_t idx, VantaValue *out) {
    const VantaValue *e = arr_peek(vm, handle, idx);
    if (!e) return false;
    *out = *e;
    value_retain(vm, *out);
    return true;
}

static bool arr_set(VantaVM *vm, uint32_t handle, uint32_t idx, VantaValue v) {
    if (!arr_head_valid(vm, handle)) { value_release(vm, v); return false; }
    VantaArrHead *h = &vm->arr_heads[handle];
    if (idx >= h->len) { value_release(vm, v); return false; }
    VantaValue *e = arr_elems(vm, handle);
    value_release(vm, e[idx]);       /* displaced element dies here */
    value_retain(vm, v);
    e[idx] = v;
    return true;
}

static bool arr_push(VantaVM *vm, uint32_t handle, VantaValue v) {
    if (!arr_head_valid(vm, handle)) { value_release(vm, v); return false; }
    VantaArrHead *h = &vm->arr_heads[handle];
    if (h->len >= h->cap) {
        if (!arr_grow(vm, handle, h->len + 1)) { value_release(vm, v); return false; }
        h = &vm->arr_heads[handle];
    }
    value_retain(vm, v);
    arr_elems(vm, handle)[h->len++] = v;
    return true;
}

static bool arr_pop(VantaVM *vm, uint32_t handle, VantaValue *out) {
    if (!arr_head_valid(vm, handle)) return false;
    VantaArrHead *h = &vm->arr_heads[handle];
    if (h->len == 0) return false;
    VantaValue v = arr_elems(vm, handle)[--h->len];
    if (out) *out = v;               /* ownership transferred */
    else value_release(vm, v);
    return true;
}

static bool arr_insert(VantaVM *vm, uint32_t handle, uint32_t idx, VantaValue v) {
    if (!arr_head_valid(vm, handle)) { value_release(vm, v); return false; }
    VantaArrHead *h = &vm->arr_heads[handle];
    if (idx > h->len) { value_release(vm, v); return false; }
    if (h->len >= h->cap) {
        if (!arr_grow(vm, handle, h->len + 1)) { value_release(vm, v); return false; }
        h = &vm->arr_heads[handle];
    }
    VantaValue *e = arr_elems(vm, handle);
    if (h->len > idx) {
        /* overlapping shift right by one slot: v_memcopy walks backwards for
         * dst > src, which is exactly this case */
        v_memcpy(&e[idx + 1], &e[idx], (h->len - idx) * sizeof(VantaValue));
    }
    value_retain(vm, v);
    e[idx] = v;
    h->len++;
    return true;
}

static bool arr_remove_at(VantaVM *vm, uint32_t handle, uint32_t idx, VantaValue *out) {
    if (!arr_head_valid(vm, handle)) return false;
    VantaArrHead *h = &vm->arr_heads[handle];
    if (idx >= h->len) return false;
    VantaValue *e = arr_elems(vm, handle);
    VantaValue v = e[idx];
    if (idx + 1 < h->len) {
        v_memcpy(&e[idx], &e[idx + 1], (h->len - idx - 1) * sizeof(VantaValue));
    }
    h->len--;
    if (out) *out = v;               /* ownership transferred */
    else value_release(vm, v);
    return true;
}

/* remove the first element equal to v (equality per value_equal). Returns
 * true when something was removed. v is not consumed. */
static bool arr_remove_value(VantaVM *vm, uint32_t handle, VantaValue v) {
    if (!arr_head_valid(vm, handle)) return false;
    uint32_t len = vm->arr_heads[handle].len;
    for (uint32_t i = 0; i < len; i++) {
        VantaValue *e = arr_elems(vm, handle);
        if (value_equal(vm, e[i], v)) {
            arr_remove_at(vm, handle, i, NULL);
            return true;
        }
    }
    return false;
}

static bool arr_index_of(VantaVM *vm, uint32_t handle, VantaValue v, int64_t *out) {
    if (!arr_head_valid(vm, handle)) return false;
    uint32_t len = vm->arr_heads[handle].len;
    const VantaValue *e = arr_elems(vm, handle);
    for (uint32_t i = 0; i < len; i++) {
        if (value_equal(vm, e[i], v)) { *out = (int64_t)i; return true; }
    }
    *out = -1;
    return true;
}

static uint32_t arr_concat(VantaVM *vm, uint32_t a, uint32_t b) {
    if (!arr_head_valid(vm, a) || !arr_head_valid(vm, b)) return VANTA_NIL_HANDLE;
    uint32_t la = vm->arr_heads[a].len;
    uint32_t lb = vm->arr_heads[b].len;
    if (la > VANTA_ARR_MAX || lb > VANTA_ARR_MAX || la + lb > VANTA_ARR_MAX) {
        return VANTA_NIL_HANDLE;      /* served limit: report OOM upstream */
    }
    uint32_t h = arr_new(vm, la + lb);
    if (h == VANTA_NIL_HANDLE) return VANTA_NIL_HANDLE;

    const VantaValue *ea = arr_elems(vm, a);
    const VantaValue *eb = arr_elems(vm, b);
    for (uint32_t i = 0; i < la; i++) {
        if (!arr_push(vm, h, ea[i])) { arr_release(vm, h); return VANTA_NIL_HANDLE; }
    }
    for (uint32_t i = 0; i < lb; i++) {
        if (!arr_push(vm, h, eb[i])) { arr_release(vm, h); return VANTA_NIL_HANDLE; }
    }
    return h;
}

/* Python-style slice semantics: negative indices count from the end, both
 * ends clamp, from >= to yields the empty array. */
static uint32_t arr_slice(VantaVM *vm, uint32_t handle, int64_t from, int64_t to, bool *ok) {
    *ok = false;
    if (!arr_head_valid(vm, handle)) return VANTA_NIL_HANDLE;
    int64_t len = (int64_t)vm->arr_heads[handle].len;

    if (from < 0) from += len;
    if (to < 0)   to += len;
    from = i64_clamp(from, 0, len);
    to   = i64_clamp(to, 0, len);
    if (from > to) from = to;

    uint32_t n = (uint32_t)(to - from);
    uint32_t h = arr_new(vm, n);
    if (h == VANTA_NIL_HANDLE) return VANTA_NIL_HANDLE;

    const VantaValue *e = arr_elems(vm, handle);
    for (uint32_t i = 0; i < n; i++) {
        if (!arr_push(vm, h, e[from + i])) { arr_release(vm, h); return VANTA_NIL_HANDLE; }
    }
    *ok = true;
    return h;
}

/* sorting
 * Insertion sort below 12 elements, median-of-three quicksort above it with
 * an explicit range stack (no recursion: the kernel stack is 16 KB). Depth
 * guard: after 2*log2(n) partitions the remaining range is insertion-sorted,
 * which kills the O(n^2) degenerate case on already-ordered input. */

#define SORT_INSERTION_MAX 12u
#define SORT_STACK_MAX     64u

typedef struct { uint32_t lo; uint32_t hi; uint32_t depth; } SortRange;

static void sort_insertion(VantaVM *vm, VantaValue *a, uint32_t lo, uint32_t hi) {
    for (uint32_t i = lo + 1; i <= hi; i++) {
        VantaValue key = a[i];
        uint32_t j = i;
        while (j > lo) {
            bool ok;
            int c = value_compare(vm, a[j - 1], key, &ok);
            if (!ok) c = 0;            /* defensive: compare of equal types never fails */
            if (c <= 0) break;
            a[j] = a[j - 1];
            j--;
        }
        a[j] = key;
    }
}

static uint32_t sort_partition(VantaVM *vm, VantaValue *a, uint32_t lo, uint32_t hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    bool ok;
    VantaValue t;

    /* order the triple a[lo] <= a[mid] <= a[hi]; ranges reaching this
     * function always hold >= 3 elements */
    if (value_compare(vm, a[lo], a[mid], &ok) > 0) { t = a[lo];  a[lo]  = a[mid]; a[mid] = t; }
    if (value_compare(vm, a[lo], a[hi],  &ok) > 0) { t = a[lo];  a[lo]  = a[hi];  a[hi]  = t; }
    if (value_compare(vm, a[mid], a[hi], &ok) > 0) { t = a[mid]; a[mid] = a[hi];  a[hi]  = t; }

    /* move the median to the pivot position and partition around it */
    t = a[mid]; a[mid] = a[hi]; a[hi] = t;
    VantaValue pivot = a[hi];

    uint32_t i = lo;
    for (uint32_t j = lo; j < hi; j++) {
        bool ok2;
        if (value_compare(vm, a[j], pivot, &ok2) < 0) {
            t = a[i]; a[i] = a[j]; a[j] = t;
            i++;
        }
    }
    t = a[i]; a[i] = a[hi]; a[hi] = t;
    return i;
}

static void arr_sort(VantaVM *vm, uint32_t handle) {
    if (!arr_head_valid(vm, handle)) return;
    uint32_t n = vm->arr_heads[handle].len;
    if (n < 2) return;
    VantaValue *a = arr_elems(vm, handle);

    if (n <= SORT_INSERTION_MAX) { sort_insertion(vm, a, 0, n - 1); return; }

    /* depth limit 2*log2(n) ~= 20 for n <= 1024 */
    uint32_t depth_limit = 0;
    {
        uint32_t m = n;
        while (m > 1) { m >>= 1; depth_limit++; }
        depth_limit *= 2;
    }

    SortRange stack[SORT_STACK_MAX];
    uint32_t top = 0;
    stack[top].lo = 0;
    stack[top].hi = n - 1;
    stack[top].depth = 0;
    top++;

    while (top) {
        top--;
        uint32_t lo = stack[top].lo;
        uint32_t hi = stack[top].hi;
        uint32_t depth = stack[top].depth;

        while (hi > lo) {
            if (hi - lo + 1 <= SORT_INSERTION_MAX || depth >= depth_limit) {
                sort_insertion(vm, a, lo, hi);
                break;
            }
            uint32_t p = sort_partition(vm, a, lo, hi);
            /* recurse into the smaller side, iterate the larger: keeps the
             * explicit stack at O(log n) deep */
            if (p > lo && p - lo < hi - p) {
                if (top < SORT_STACK_MAX) {
                    stack[top].lo = p + 1;
                    stack[top].hi = hi;
                    stack[top].depth = depth + 1;
                    top++;
                    hi = p - 1;
                } else {
                    sort_insertion(vm, a, lo, hi);
                    break;
                }
            } else if (p < hi && hi - p < p - lo) {
                if (top < SORT_STACK_MAX) {
                    stack[top].lo = lo;
                    stack[top].hi = (p > lo) ? p - 1 : lo;
                    stack[top].depth = depth + 1;
                    top++;
                    lo = p + 1;
                } else {
                    sort_insertion(vm, a, lo, hi);
                    break;
                }
            } else {
                sort_insertion(vm, a, lo, hi);
                break;
            }
            depth++;
        }
    }
}

static void arr_reverse(VantaVM *vm, uint32_t handle) {
    if (!arr_head_valid(vm, handle)) return;
    uint32_t n = vm->arr_heads[handle].len;
    VantaValue *a = arr_elems(vm, handle);
    for (uint32_t i = 0; i + 1 < n; i++, n--) {
        VantaValue t = a[i];
        a[i] = a[n - 1];
        a[n - 1] = t;
    }
}

static bool arr_fill(VantaVM *vm, uint32_t handle, uint32_t from, uint32_t to, VantaValue v) {
    if (!arr_head_valid(vm, handle)) { value_release(vm, v); return false; }
    uint32_t len = vm->arr_heads[handle].len;
    if (from > len) from = len;
    if (to > len) to = len;
    VantaValue *a = arr_elems(vm, handle);
    value_retain(vm, v);
    for (uint32_t i = from; i < to; i++) {
        value_release(vm, a[i]);
        a[i] = v;                        /* one retain serves all slots: each
                                          * slot now holds a reference, and the
                                          * initial retain above covers the last */
        value_retain(vm, v);
    }
    value_release(vm, v);                /* drop the seed reference */
    return true;
}

/* join elements into one string with a separator; elements render through
 * value_to_sb exactly like println does. Returns NIL_HANDLE on OOM. */
static uint32_t arr_join(VantaVM *vm, uint32_t handle, uint32_t sep_handle) {
    if (!arr_head_valid(vm, handle)) return VANTA_NIL_HANDLE;
    uint32_t n = vm->arr_heads[handle].len;
    const VantaValue *a = arr_elems(vm, handle);
    const char *sep = (sep_handle != VANTA_NIL_HANDLE) ? str_data(vm, sep_handle) : "";
    uint32_t sep_len = (sep_handle != VANTA_NIL_HANDLE) ? str_len(vm, sep_handle) : 0;

    sb_reset(&vm->sb);
    for (uint32_t i = 0; i < n; i++) {
        if (i && sep_len) {
            if (!sb_append(&vm->sb, sep, sep_len)) return VANTA_NIL_HANDLE;
        }
        if (!sb_append_value(vm, &vm->sb, a[i])) return VANTA_NIL_HANDLE;
    }
    return str_take_sb(vm, &vm->sb);
}

/* P10 - object pool: string-keyed maps
 *
 * Open addressing, linear probing, tombstone deletion, load factor capped
 * at 0.7. Buckets live in obj_arena size-class chunks and double on growth
 * (8 -> 256 buckets max). Ownership follows the array contract:
 *   map_set retains key + value, releases a displaced value
 *   map_get copies AND retains the value
 *   map_delete releases key + value (unless *out takes ownership)
 *   map_new returns refs = 1, owned by the caller
 * Keys are always pooled strings; map keys() returns a new array. */

static void map_release(VantaVM *vm, uint32_t handle);   /* fwd: defined in P11 */

static bool map_head_valid(VantaVM *vm, uint32_t handle) {
    return handle != VANTA_NIL_HANDLE && handle < VANTA_MAP_HEADS &&
           vm->map_heads[handle].refs > 0;
}

static VantaMapBucket *map_buckets(VantaVM *vm, uint32_t handle) {
    return (VantaMapBucket *)(vm->obj_arena + vm->map_heads[handle].data_off);
}

/* bucket capacities are powers of two so masking replaces division */
static uint32_t map_round_cap(uint32_t want) {
    uint32_t c = 8;
    while (c < want && c < VANTA_MAP_BUCKETS_MAX) c *= 2;
    return c;
}

static uint32_t map_alloc_buckets(VantaVM *vm, uint32_t cap, uint32_t *cls_out) {
    uint32_t bytes = cap * (uint32_t)sizeof(VantaMapBucket);
    return arena_alloc(vm->obj_arena, VANTA_OBJ_ARENA, &vm->obj_arena_used,
                       vm->obj_class_free, VANTA_OBJ_CLASSES,
                       obj_class_sizes, bytes, cls_out);
}

static void map_init_buckets(VantaVM *vm, uint32_t handle) {
    VantaMapBucket *b = map_buckets(vm, handle);
    uint32_t cap = vm->map_heads[handle].cap;
    for (uint32_t i = 0; i < cap; i++) {
        b[i].key = VANTA_EMPTY_BUCKET;
        b[i].pad = 0;
        b[i].val = VANTA_VALUE_NIL;
    }
}

static uint32_t map_new(VantaVM *vm, uint32_t cap_hint) {
    if (vm->map_head_free == VANTA_NIL_HANDLE) return VANTA_NIL_HANDLE;

    uint32_t cap = map_round_cap(cap_hint == 0 ? 8 : cap_hint);
    uint32_t cls;
    uint32_t off = map_alloc_buckets(vm, cap, &cls);
    if (off == VANTA_NIL_HANDLE) return VANTA_NIL_HANDLE;

    uint32_t handle = vm->map_head_free;
    vm->map_head_free = vm->map_heads[handle].data_off;

    VantaMapHead *h = &vm->map_heads[handle];
    h->refs = 1;
    h->count = 0;
    h->cap = cap;
    h->data_off = off;
    h->size_cls = cls;
    map_init_buckets(vm, handle);
    vm->map_live++;
    return handle;
}

static void map_retain(VantaVM *vm, uint32_t handle) {
    if (!map_head_valid(vm, handle)) return;
    VantaMapHead *h = &vm->map_heads[handle];
    if (h->refs < 0xFFFFFFFFu) h->refs++;
}

static uint32_t map_count(VantaVM *vm, uint32_t handle) {
    if (!map_head_valid(vm, handle)) return 0;
    return vm->map_heads[handle].count;
}

/* Probe for `key`. On return:
 *   *found       true when the key is present; *slot holds its bucket
 *   *found       false: *slot holds the best insertion bucket (first
 *                tombstone on the probe path, else the empty terminator),
 *                or NO_INDEX when the table is pathologically full */
static void map_probe(VantaVM *vm, uint32_t handle, uint32_t key,
                      bool *found, uint32_t *slot) {
    *found = false;
    *slot = VANTA_NO_INDEX;

    uint32_t hash = str_hash_of(vm, key);
    uint32_t cap = vm->map_heads[handle].cap;
    uint32_t mask = cap - 1;
    VantaMapBucket *b = map_buckets(vm, handle);

    uint32_t i = hash & mask;
    uint32_t first_tomb = VANTA_NO_INDEX;
    for (uint32_t step = 0; step < cap; step++) {
        if (b[i].key == VANTA_EMPTY_BUCKET) {
            *slot = (first_tomb != VANTA_NO_INDEX) ? first_tomb : i;
            return;
        }
        if (b[i].key == VANTA_TOMB_BUCKET) {
            if (first_tomb == VANTA_NO_INDEX) first_tomb = i;
        } else if (b[i].key == key || str_equal(vm, b[i].key, key)) {
            *found = true;
            *slot = i;
            return;
        }
        i = (i + 1) & mask;
    }
    /* every bucket holds a live key or a tombstone (cannot happen below
     * load factor 0.7, but fail safe rather than loop forever) */
    *slot = first_tomb;
}

/* Double the bucket array and rehash. Ownership of keys/values transfers -
 * no retain/release traffic, the entries move wholesale. */
static bool map_grow(VantaVM *vm, uint32_t handle) {
    VantaMapHead *h = &vm->map_heads[handle];
    if (h->cap >= VANTA_MAP_BUCKETS_MAX) return false;

    uint32_t new_cap = h->cap * 2;
    uint32_t cls;
    uint32_t off = map_alloc_buckets(vm, new_cap, &cls);
    if (off == VANTA_NIL_HANDLE) return false;

    VantaMapBucket *old_b = map_buckets(vm, handle);
    uint32_t old_cap = h->cap;
    uint32_t old_off = h->data_off;
    uint32_t old_cls = h->size_cls;

    h->data_off = off;
    h->size_cls = cls;
    h->cap = new_cap;
    map_init_buckets(vm, handle);

    VantaMapBucket *new_b = map_buckets(vm, handle);
    uint32_t new_mask = new_cap - 1;
    for (uint32_t i = 0; i < old_cap; i++) {
        if (old_b[i].key == VANTA_EMPTY_BUCKET || old_b[i].key == VANTA_TOMB_BUCKET)
            continue;
        uint32_t hash = str_hash_of(vm, old_b[i].key);
        uint32_t j = hash & new_mask;
        while (new_b[j].key != VANTA_EMPTY_BUCKET) j = (j + 1) & new_mask;
        new_b[j].key = old_b[i].key;      /* ownership transfers */
        new_b[j].pad = 0;
        new_b[j].val = old_b[i].val;
    }

    arena_free_chunk(vm->obj_arena, old_off, vm->obj_class_free, old_cls);
    return true;
}

/* Set key -> value. BORROWS key and value: the map takes its own references
 * (retains) and the caller keeps - and must later release - whatever refs it
 * came in with. Returns false on capacity exhaustion; on false the map holds
 * no references and the caller must clean up its own. */
static bool map_set(VantaVM *vm, uint32_t handle, uint32_t key, VantaValue v) {
    if (!map_head_valid(vm, handle) || !str_head_valid(vm, key)) {
        return false;
    }
    VantaMapHead *h = &vm->map_heads[handle];

    bool found;
    uint32_t slot;
    map_probe(vm, handle, key, &found, &slot);

    if (!found) {
        if (slot == VANTA_NO_INDEX) return false;
        /* inserting over a tombstone does not grow count; a fresh bucket does */
        bool over_tomb = (map_buckets(vm, handle)[slot].key == VANTA_TOMB_BUCKET);
        if (!over_tomb && (h->count + 1) * 10 > h->cap * 7) {
            if (!map_grow(vm, handle)) return false;
            /* re-probe: the grow rehashed everything into new slots */
            map_probe(vm, handle, key, &found, &slot);
            if (found || slot == VANTA_NO_INDEX) return false;
        }
        VantaMapBucket *b = map_buckets(vm, handle);
        value_retain(vm, v);
        str_retain(vm, key);
        b[slot].key = key;
        b[slot].pad = 0;
        b[slot].val = v;
        h->count++;
        return true;
    }

    VantaMapBucket *b = map_buckets(vm, handle);
    value_release(vm, b[slot].val);
    value_retain(vm, v);
    b[slot].val = v;
    return true;
}

static bool map_get(VantaVM *vm, uint32_t handle, uint32_t key,
                    VantaValue *out, bool *found) {
    *found = false;
    *out = VANTA_VALUE_NIL;
    if (!map_head_valid(vm, handle) || !str_head_valid(vm, key)) return false;

    bool hit;
    uint32_t slot;
    map_probe(vm, handle, key, &hit, &slot);
    if (!hit) return true;             /* probe worked, key simply absent */

    *found = true;
    *out = map_buckets(vm, handle)[slot].val;
    value_retain(vm, *out);
    return true;
}

/* Delete key. When out != NULL the removed value transfers to *out and the
 * caller owns it; otherwise it is released here. The key string is always
 * released. */
static bool map_delete(VantaVM *vm, uint32_t handle, uint32_t key,
                       bool *found, VantaValue *out) {
    if (found) *found = false;
    if (!map_head_valid(vm, handle)) return false;

    bool hit;
    uint32_t slot;
    map_probe(vm, handle, key, &hit, &slot);
    if (!hit) return true;

    VantaMapBucket *b = map_buckets(vm, handle);
    str_release(vm, b[slot].key);
    if (out) {
        *out = b[slot].val;            /* ownership transferred */
    } else {
        value_release(vm, b[slot].val);
    }
    b[slot].key = VANTA_TOMB_BUCKET;
    b[slot].pad = 0;
    b[slot].val = VANTA_VALUE_NIL;
    vm->map_heads[handle].count--;
    if (found) *found = true;
    return true;
}

static bool map_has(VantaVM *vm, uint32_t handle, uint32_t key) {
    if (!map_head_valid(vm, handle) || !str_head_valid(vm, key)) return false;
    bool hit;
    uint32_t slot;
    map_probe(vm, handle, key, &hit, &slot);
    return hit;
}

/* Release every entry and reset probing state. count -> 0, buckets -> empty. */
static void map_clear(VantaVM *vm, uint32_t handle) {
    if (!map_head_valid(vm, handle)) return;
    VantaMapBucket *b = map_buckets(vm, handle);
    uint32_t cap = vm->map_heads[handle].cap;
    for (uint32_t i = 0; i < cap; i++) {
        if (b[i].key != VANTA_EMPTY_BUCKET && b[i].key != VANTA_TOMB_BUCKET) {
            str_release(vm, b[i].key);
            value_release(vm, b[i].val);
        }
        b[i].key = VANTA_EMPTY_BUCKET;
        b[i].pad = 0;
        b[i].val = VANTA_VALUE_NIL;
    }
    vm->map_heads[handle].count = 0;
}

/* keys() -> new array of key strings (each retained once for the array).
 * Order is bucket order: stable for a given map state, NOT insertion order -
 * documented behavior, insertion-order maps cost a second index array the
 * kernel doesn't need. */
static uint32_t map_keys_array(VantaVM *vm, uint32_t handle) {
    if (!map_head_valid(vm, handle)) return VANTA_NIL_HANDLE;
    uint32_t n = vm->map_heads[handle].count;
    uint32_t arr = arr_new(vm, n);
    if (arr == VANTA_NIL_HANDLE) return VANTA_NIL_HANDLE;

    VantaMapBucket *b = map_buckets(vm, handle);
    uint32_t cap = vm->map_heads[handle].cap;
    for (uint32_t i = 0; i < cap; i++) {
        if (b[i].key != VANTA_EMPTY_BUCKET && b[i].key != VANTA_TOMB_BUCKET) {
            /* make_str borrows; arr_push retains for the array, the map's own
             * reference to the key stays untouched */
            VantaValue kv = make_str(b[i].key);
            if (!arr_push(vm, arr, kv)) {
                arr_release(vm, arr);
                return VANTA_NIL_HANDLE;
            }
        }
    }
    return arr;
}

static uint32_t map_values_array(VantaVM *vm, uint32_t handle) {
    if (!map_head_valid(vm, handle)) return VANTA_NIL_HANDLE;
    uint32_t n = vm->map_heads[handle].count;
    uint32_t arr = arr_new(vm, n);
    if (arr == VANTA_NIL_HANDLE) return VANTA_NIL_HANDLE;

    VantaMapBucket *b = map_buckets(vm, handle);
    uint32_t cap = vm->map_heads[handle].cap;
    for (uint32_t i = 0; i < cap; i++) {
        if (b[i].key != VANTA_EMPTY_BUCKET && b[i].key != VANTA_TOMB_BUCKET) {
            if (!arr_push(vm, arr, b[i].val)) {
                arr_release(vm, arr);
                return VANTA_NIL_HANDLE;
            }
        }
    }
    return arr;
}

/* P11 - value lifecycle: refcounts, release, truthiness, equality,
 *        ordering, and text rendering
 *
 * Release is ITERATIVE over an explicit worklist. Recursive release would
 * blow the 16 KB kernel stack on a 300-deep nested array - and nesting depth
 * is bounded by the pool itself (512 array heads + 128 map heads), so a
 * 720-slot worklist provably covers the worst case. The worklist is static
 * scratch: the VM runs one script at a time, same contract as the rest of
 * the file. */

/* constructors */

static VantaValue make_int(int64_t v) {
    VantaValue r;
    r.type = V_INT;
    r.pad[0] = r.pad[1] = r.pad[2] = 0;
    r.as.i = v;
    return r;
}

static VantaValue make_float(double v) {
    VantaValue r;
    r.type = V_FLOAT;
    r.pad[0] = r.pad[1] = r.pad[2] = 0;
    r.as.f = v;
    return r;
}

static VantaValue make_bool(bool v) {
    return v ? VANTA_VALUE_TRUE : VANTA_VALUE_FALSE;
}

static VantaValue make_str(uint32_t handle) {
    VantaValue r;
    r.type = V_STR;
    r.pad[0] = r.pad[1] = r.pad[2] = 0;
    r.as.h = handle;
    return r;
}

static VantaValue make_arr(uint32_t handle) {
    VantaValue r;
    r.type = V_ARR;
    r.pad[0] = r.pad[1] = r.pad[2] = 0;
    r.as.h = handle;
    return r;
}

static VantaValue make_map(uint32_t handle) {
    VantaValue r;
    r.type = V_MAP;
    r.pad[0] = r.pad[1] = r.pad[2] = 0;
    r.as.h = handle;
    return r;
}

static VantaValue make_func(uint32_t idx) {
    VantaValue r;
    r.type = V_FUNC;
    r.pad[0] = r.pad[1] = r.pad[2] = 0;
    r.as.i = (int64_t)idx;
    return r;
}

static VantaValue make_native(uint32_t idx) {
    VantaValue r;
    r.type = V_NATIVE;
    r.pad[0] = r.pad[1] = r.pad[2] = 0;
    r.as.i = (int64_t)idx;
    return r;
}

/* alloc + wrap; returns V_NIL value when the pool refuses (caller checks) */
static VantaValue str_value_from(VantaVM *vm, const char *bytes, uint32_t len) {
    uint32_t h = str_alloc(vm, bytes, len);
    if (h == VANTA_NIL_HANDLE && len > 0) return VANTA_VALUE_NIL;
    return make_str(h);
}

/* retain / release */

static void value_retain(VantaVM *vm, VantaValue v) {
    switch (v.type) {
        case V_STR: str_retain(vm, v.as.h); break;
        case V_ARR: arr_retain(vm, v.as.h); break;
        case V_MAP: map_retain(vm, v.as.h); break;
        default: break;
    }
}

#define RELEASE_WORKLIST_CAP 720u

typedef struct {
    uint8_t type;                 /* V_ARR or V_MAP: only containers enqueue */
    uint8_t pad[3];
    uint32_t handle;
} ReleaseItem;

static ReleaseItem release_worklist[RELEASE_WORKLIST_CAP];
static uint32_t    release_worklist_len = 0;

/* container teardown with refs already at zero: drops element references,
 * frees the storage chunk, recycles the head slot */
static void release_arr_fully(VantaVM *vm, uint32_t handle) {
    VantaArrHead *h = &vm->arr_heads[handle];
    uint32_t n = h->len;
    if (n) {
        VantaValue *e = arr_elems(vm, handle);
        for (uint32_t i = 0; i < n; i++) {
            switch (e[i].type) {
                case V_STR:
                    str_release(vm, e[i].as.h);
                    break;
                case V_ARR: case V_MAP: {
                    /* decrement; only enqueue containers that reach zero */
                    uint32_t ch = e[i].as.h;
                    if (e[i].type == V_ARR ? arr_head_valid(vm, ch) : map_head_valid(vm, ch)) {
                        if (e[i].type == V_ARR) {
                            VantaArrHead *c = &vm->arr_heads[ch];
                            if (--c->refs == 0 && release_worklist_len < RELEASE_WORKLIST_CAP) {
                                release_worklist[release_worklist_len].type = V_ARR;
                                release_worklist[release_worklist_len].handle = ch;
                                release_worklist_len++;
                            }
                        } else {
                            VantaMapHead *c = &vm->map_heads[ch];
                            if (--c->refs == 0 && release_worklist_len < RELEASE_WORKLIST_CAP) {
                                release_worklist[release_worklist_len].type = V_MAP;
                                release_worklist[release_worklist_len].handle = ch;
                                release_worklist_len++;
                            }
                        }
                    }
                    break;
                }
                default:
                    break;
            }
        }
    }
    arena_free_chunk(vm->obj_arena, h->data_off, vm->obj_class_free, h->size_cls);
    h->refs = 0;
    h->data_off = vm->arr_head_free;
    vm->arr_head_free = handle;
    vm->arr_live--;
}

static void release_map_fully(VantaVM *vm, uint32_t handle) {
    VantaMapHead *h = &vm->map_heads[handle];
    uint32_t cap = h->cap;
    VantaMapBucket *b = map_buckets(vm, handle);
    for (uint32_t i = 0; i < cap; i++) {
        if (b[i].key == VANTA_EMPTY_BUCKET || b[i].key == VANTA_TOMB_BUCKET) continue;
        str_release(vm, b[i].key);
        VantaValue v = b[i].val;
        switch (v.type) {
            case V_STR:
                str_release(vm, v.as.h);
                break;
            case V_ARR: case V_MAP: {
                uint32_t ch = v.as.h;
                if (v.type == V_ARR ? arr_head_valid(vm, ch) : map_head_valid(vm, ch)) {
                    if (v.type == V_ARR) {
                        VantaArrHead *c = &vm->arr_heads[ch];
                        if (--c->refs == 0 && release_worklist_len < RELEASE_WORKLIST_CAP) {
                            release_worklist[release_worklist_len].type = V_ARR;
                            release_worklist[release_worklist_len].handle = ch;
                            release_worklist_len++;
                        }
                    } else {
                        VantaMapHead *c = &vm->map_heads[ch];
                        if (--c->refs == 0 && release_worklist_len < RELEASE_WORKLIST_CAP) {
                            release_worklist[release_worklist_len].type = V_MAP;
                            release_worklist[release_worklist_len].handle = ch;
                            release_worklist_len++;
                        }
                    }
                }
                break;
            }
            default:
                break;
        }
    }
    arena_free_chunk(vm->obj_arena, h->data_off, vm->obj_class_free, h->size_cls);
    h->refs = 0;
    h->data_off = vm->map_head_free;
    vm->map_head_free = handle;
    vm->map_live--;
}

static void value_release(VantaVM *vm, VantaValue v) {
    if (v.type != V_ARR && v.type != V_MAP) {
        if (v.type == V_STR) str_release(vm, v.as.h);
        return;
    }

    /* top-level container: fast decrement; alive means we are done */
    if (v.type == V_ARR) {
        if (v.as.h == VANTA_NIL_HANDLE || v.as.h >= VANTA_ARR_HEADS) return;
        VantaArrHead *h = &vm->arr_heads[v.as.h];
        if (h->refs == 0) return;                    /* stale: refuse */
        if (--h->refs > 0) return;
        release_worklist_len = 0;
        release_arr_fully(vm, v.as.h);
    } else {
        if (v.as.h == VANTA_NIL_HANDLE || v.as.h >= VANTA_MAP_HEADS) return;
        VantaMapHead *h = &vm->map_heads[v.as.h];
        if (h->refs == 0) return;
        if (--h->refs > 0) return;
        release_worklist_len = 0;
        release_map_fully(vm, v.as.h);
    }

    /* drain containers that reached zero during element teardown */
    while (release_worklist_len) {
        ReleaseItem item = release_worklist[--release_worklist_len];
        if (item.type == V_ARR) release_arr_fully(vm, item.handle);
        else                    release_map_fully(vm, item.handle);
    }
}

/* truthiness */

static bool value_truthy(VantaVM *vm, VantaValue v) {
    switch (v.type) {
        case V_NIL:    return false;
        case V_BOOL:   return v.as.i != 0;
        case V_INT:    return v.as.i != 0;
        case V_FLOAT:  return v.as.f != 0.0 && !m_isnan(v.as.f);   /* NaN is false */
        case V_STR:    return str_len(vm, v.as.h) > 0;
        case V_ARR:    return arr_len(vm, v.as.h) > 0;
        case V_MAP:    return map_count(vm, v.as.h) > 0;
        case V_FUNC:   return true;
        case V_NATIVE: return true;
        default:       return false;
    }
}

/* equality */

static bool value_equal(VantaVM *vm, VantaValue a, VantaValue b) {
    /* numeric equality crosses the int/float boundary */
    bool an = (a.type == V_INT || a.type == V_FLOAT);
    bool bn = (b.type == V_INT || b.type == V_FLOAT);
    if (an && bn) {
        double da = (a.type == V_INT) ? (double)a.as.i : a.as.f;
        double db = (b.type == V_INT) ? (double)b.as.i : b.as.f;
        return da == db;    /* NaN != NaN falls out of IEEE ordering */
    }
    if (a.type != b.type) return false;
    switch (a.type) {
        case V_NIL:    return true;
        case V_BOOL:   return a.as.i == b.as.i;
        case V_STR:    return str_equal(vm, a.as.h, b.as.h);
        case V_ARR:    return a.as.h == b.as.h;   /* identity: deep == is */
        case V_MAP:    return a.as.h == b.as.h;   /* quadratic; documented */
        case V_FUNC:   return a.as.i == b.as.i;
        case V_NATIVE: return a.as.i == b.as.i;
        default:       return false;
    }
}

/* ordering
 * ok=false means "not ordered by the language": cross-type comparisons and
 * NaN. The sort path treats ok=false as "equal" (stable no-swap), the
 * operator path turns it into a typed runtime error. Type rank gives a TOTAL
 * order anyway when the caller insists (sorting mixed arrays). */

static uint32_t value_type_rank(VantaType t) {
    switch (t) {
        case V_NIL:    return 0;
        case V_BOOL:   return 1;
        case V_INT:    return 2;
        case V_FLOAT:  return 2;       /* ints and floats share the numeric rank */
        case V_STR:    return 3;
        case V_ARR:    return 4;
        case V_MAP:    return 5;
        case V_FUNC:   return 6;
        case V_NATIVE: return 6;
        default:       return 7;
    }
}

static int value_compare(VantaVM *vm, VantaValue a, VantaValue b, bool *ok) {
    *ok = true;

    bool an = (a.type == V_INT || a.type == V_FLOAT);
    bool bn = (b.type == V_INT || b.type == V_FLOAT);
    if (an && bn) {
        double da = (a.type == V_INT) ? (double)a.as.i : a.as.f;
        double db = (b.type == V_INT) ? (double)b.as.i : b.as.f;
        if (m_isnan(da) || m_isnan(db)) { *ok = false; return 0; }
        if (da < db) return -1;
        if (da > db) return 1;
        return 0;
    }

    if (a.type == V_STR && b.type == V_STR) {
        int32_t c = str_compare(vm, a.as.h, b.as.h);
        return c < 0 ? -1 : (c > 0 ? 1 : 0);
    }

    if (a.type == b.type && a.type == V_BOOL) {
        return a.as.i < b.as.i ? -1 : (a.as.i > b.as.i ? 1 : 0);
    }

    /* different ranks: total order by rank, but NOT language-comparable */
    if (a.type != b.type) {
        *ok = false;
        uint32_t ra = value_type_rank(a.type);
        uint32_t rb = value_type_rank(b.type);
        return ra < rb ? -1 : 1;
    }
    return 0;
}

static const char *value_type_name(VantaValue v) {
    switch (v.type) {
        case V_NIL:    return "nil";
        case V_BOOL:   return "bool";
        case V_INT:    return "int";
        case V_FLOAT:  return "float";
        case V_STR:    return "string";
        case V_ARR:    return "array";
        case V_MAP:    return "map";
        case V_FUNC:   return "function";
        case V_NATIVE: return "native";
        default:       return "unknown";
    }
}

/* rendering
 * println(str(x)) and error message building share this renderer. Container
 * rendering is depth-limited: a self-referential array (`let a = []; a.push(a)`)
 * cannot recurse infinitely and prints as `[...]` at depth 5. Cycles leak their
 * refcounts by design - documented tradeoff, the pools catch the exhaustion. */

#define VALUE_PRINT_DEPTH_MAX 5u

static void value_to_sb_depth(VantaVM *vm, VantaSB *sb, VantaValue v, uint32_t depth);

static void value_to_sb(VantaVM *vm, VantaSB *sb, VantaValue v) {
    value_to_sb_depth(vm, sb, v, 0);
}

static void value_to_sb_depth(VantaVM *vm, VantaSB *sb, VantaValue v, uint32_t depth) {
    char nbuf[48];
    switch (v.type) {
        case V_NIL:
            sb_append(sb, "nil", 3);
            break;
        case V_BOOL:
            if (v.as.i) sb_append(sb, "true", 4);
            else        sb_append(sb, "false", 5);
            break;
        case V_INT: {
            uint32_t n = v_fmt_i64(nbuf, v.as.i, 10, false, false);
            sb_append(sb, nbuf, n);
            break;
        }
        case V_FLOAT: {
            /* 17 significant digits: shortest faithful round-trip */
            uint32_t n = v_fmt_f64_g(nbuf, sizeof(nbuf), v.as.f, 17, false);
            (void)n;
            sb_append(sb, nbuf, (uint32_t)v_strlen(nbuf));
            break;
        }
        case V_STR:
            sb_append(sb, str_data(vm, v.as.h), str_len(vm, v.as.h));
            break;
        case V_ARR: {
            if (depth >= VALUE_PRINT_DEPTH_MAX) { sb_append(sb, "[...]", 5); break; }
            uint32_t n = arr_len(vm, v.as.h);
            sb_append_char(sb, '[');
            for (uint32_t i = 0; i < n; i++) {
                if (i) sb_append(sb, ", ", 2);
                const VantaValue *e = arr_peek(vm, v.as.h, i);
                if (e) value_to_sb_depth(vm, sb, *e, depth + 1);
            }
            sb_append_char(sb, ']');
            break;
        }
        case V_MAP: {
            if (depth >= VALUE_PRINT_DEPTH_MAX) { sb_append(sb, "{...}", 5); break; }
            if (!map_head_valid(vm, v.as.h)) { sb_append(sb, "{invalid}", 9); break; }
            sb_append_char(sb, '{');
            bool first = true;
            VantaMapBucket *b = map_buckets(vm, v.as.h);
            uint32_t cap = vm->map_heads[v.as.h].cap;
            for (uint32_t i = 0; i < cap; i++) {
                if (b[i].key == VANTA_EMPTY_BUCKET || b[i].key == VANTA_TOMB_BUCKET) continue;
                if (!first) sb_append(sb, ", ", 2);
                first = false;
                sb_append_char(sb, '"');
                sb_append(sb, str_data(vm, b[i].key), str_len(vm, b[i].key));
                sb_append(sb, "\": ", 3);
                value_to_sb_depth(vm, sb, b[i].val, depth + 1);
            }
            sb_append_char(sb, '}');
            break;
        }
        case V_FUNC: {
            uint32_t fi = (uint32_t)v.as.i;
            if (fi < vm->function_count && vm->functions[fi].name_idx != VANTA_NO_INDEX) {
                uint32_t ni = vm->functions[fi].name_idx;
                sb_append(sb, "<fn ", 4);
                sb_append(sb, vm->names[ni].name, vm->names[ni].len);
                sb_append_char(sb, '>');
            } else {
                sb_append(sb, "<fn>", 4);
            }
            break;
        }
        case V_NATIVE: {
            uint32_t ni = (uint32_t)v.as.i;
            const char *nm = native_name(ni);
            sb_append(sb, "<native ", 8);
            sb_append_cstr(sb, nm ? nm : "?");
            sb_append_char(sb, '>');
            break;
        }
        default:
            sb_append(sb, "<unknown>", 9);
            break;
    }
}

/* SB bridge declared in the P08 section, defined here where value_to_sb lives */
static bool sb_append_value(VantaVM *vm, VantaSB *sb, VantaValue v) {
    uint32_t before = sb->len;
    value_to_sb(vm, sb, v);
    return sb->len >= before;    /* sb ops fail hard via bounds, keep API shape */
}

/* P12 - lexer
 *
 * Tokens stream into VantaVM::tokens (bounded: VANTA_MAX_TOKENS). Newlines
 * carry grammatical weight - they terminate statements - but are suppressed
 * after binary operators and opening brackets so expressions can span lines.
 * String literals are decoded HERE (escapes expanded, UTF-8 validated) and
 * registered straight into the constant pool: the parser only ever sees an
 * index, never raw quote bytes. That keeps compile and constant folding
 * free of repeated escape rescans. */

typedef struct {
    const char *src;
    uint32_t    len;
    uint32_t    pos;
    uint32_t    line;
    uint32_t    col;
} LexCursor;

static bool lex_end(const LexCursor *c) { return c->pos >= c->len; }
static char lex_peek(const LexCursor *c) { return c->pos < c->len ? c->src[c->pos] : '\0'; }
static char lex_ahead(const LexCursor *c, uint32_t off) {
    return c->pos + off < c->len ? c->src[c->pos + off] : '\0';
}

static void lex_advance(LexCursor *c) {
    if (c->pos >= c->len) return;
    if (c->src[c->pos] == '\n') { c->line++; c->col = 1; }
    else c->col++;
    c->pos++;
}

static VantaResult lex_error(VantaVM *vm, const LexCursor *c, const char *fmt, ...) {
    char msg[VANTA_DIAG_MSG];
    va_list ap;
    va_start(ap, fmt);
    int n = vanta_vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;

    VantaDiag *d = &vm->diag;
    d->result = VANTA_ERR_SYNTAX;
    d->line = c->line;
    d->col = c->col;
    d->msg_len = (uint32_t)v_strlen(msg);
    v_strlcpy(d->msg, msg, sizeof(d->msg));
    return VANTA_ERR_SYNTAX;
}

static VantaResult token_push(VantaVM *vm, uint32_t kind, uint32_t line, uint32_t col,
                              uint32_t src_off) {
    if (vm->token_count >= VANTA_MAX_TOKENS) {
        LexCursor fake;
        fake.src = vm->source; fake.len = vm->src_len; fake.pos = src_off;
        fake.line = line; fake.col = col;
        return lex_error(vm, &fake, "program too large: token limit (%u) reached",
                         VANTA_MAX_TOKENS);
    }
    VantaToken *t = &vm->tokens[vm->token_count++];
    t->kind = kind;
    t->line = line;
    t->col = col;
    t->src_off = src_off;
    t->v.i = 0;
    return VANTA_OK;
}

/* Newline suppression: an expression that ends a line before a binary
 * operator is continued, not terminated. Mirrors how every C-family
 * language feels when formatted across lines. */
static bool lex_suppress_nl_after(uint32_t kind) {
    switch (kind) {
        case TK_EOF: case TK_NL:
        case TK_PLUS: case TK_MINUS: case TK_STAR: case TK_SLASH: case TK_PERCENT:
        case TK_AMP: case TK_PIPE: case TK_CARET: case TK_SHL: case TK_SHR:
        case TK_AND: case TK_OR: case TK_BANG: case TK_TILDE:
        case TK_EQ: case TK_NE: case TK_LT: case TK_LE: case TK_GT: case TK_GE:
        case TK_ASSIGN:
        case TK_PLUS_ASSIGN: case TK_MINUS_ASSIGN: case TK_STAR_ASSIGN:
        case TK_SLASH_ASSIGN: case TK_PERCENT_ASSIGN: case TK_BAND_ASSIGN:
        case TK_BOR_ASSIGN: case TK_BXOR_ASSIGN: case TK_SHL_ASSIGN: case TK_SHR_ASSIGN:
        case TK_LPAREN: case TK_LBRACE: case TK_LBRACKET:
        case TK_COMMA: case TK_COLON: case TK_QUESTION: case TK_DOT:
            return true;
        default:
            return false;
    }
}

static uint32_t last_significant_kind(VantaVM *vm) {
    uint32_t i = vm->token_count;
    while (i > 0) {
        if (vm->tokens[i - 1].kind != TK_NL) return vm->tokens[i - 1].kind;
        i--;
    }
    return TK_EOF;
}

/* number scanning */

static int hex_digit_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Strip underscore separators; returns false on a dangling underscore. */
static bool strip_underscores(char *out, uint32_t out_cap, uint32_t *out_len,
                              const char *in, uint32_t in_len) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < in_len; i++) {
        if (in[i] == '_') {
            if (i == 0 || i + 1 >= in_len) return false;   /* leading/trailing '_': caller rejects */
            continue;
        }
        if (n + 1 >= out_cap) return false;
        out[n++] = in[i];
    }
    out[n] = '\0';
    *out_len = n;
    return true;
}

static VantaResult lex_number(VantaVM *vm, LexCursor *c) {
    uint32_t start = c->pos;
    uint32_t line = c->line, col = c->col;
    static char raw[160];      /* bounded literal span; oversized ones error */
    static char cleaned[160];
    uint32_t rn = 0;

    bool is_float = false;
    uint32_t base = 10;

    /* 0x / 0b prefixes */
    if (lex_peek(c) == '0' && (lex_ahead(c, 1) == 'x' || lex_ahead(c, 1) == 'X')) {
        lex_advance(c); lex_advance(c);
        base = 16;
        while (!lex_end(c) && (hex_digit_val(lex_peek(c)) >= 0 || lex_peek(c) == '_')) {
            if (rn + 1 >= sizeof(raw)) return lex_error(vm, c, "numeric literal too long");
            raw[rn++] = lex_peek(c);
            lex_advance(c);
        }
        if (rn == 0) return lex_error(vm, c, "hex literal has no digits");
    } else if (lex_peek(c) == '0' && (lex_ahead(c, 1) == 'b' || lex_ahead(c, 1) == 'B')) {
        lex_advance(c); lex_advance(c);
        base = 2;
        while (!lex_end(c) && (lex_peek(c) == '0' || lex_peek(c) == '1' || lex_peek(c) == '_')) {
            if (rn + 1 >= sizeof(raw)) return lex_error(vm, c, "numeric literal too long");
            raw[rn++] = lex_peek(c);
            lex_advance(c);
        }
        if (rn == 0) return lex_error(vm, c, "binary literal has no digits");
    } else {
        while (!lex_end(c) && ((lex_peek(c) >= '0' && lex_peek(c) <= '9') || lex_peek(c) == '_')) {
            if (rn + 1 >= sizeof(raw)) return lex_error(vm, c, "numeric literal too long");
            raw[rn++] = lex_peek(c);
            lex_advance(c);
        }
        /* fractional part */
        if (lex_peek(c) == '.' && lex_ahead(c, 1) >= '0' && lex_ahead(c, 1) <= '9') {
            is_float = true;
            if (rn + 1 >= sizeof(raw)) return lex_error(vm, c, "numeric literal too long");
            raw[rn++] = '.';
            lex_advance(c);
            while (!lex_end(c) && ((lex_peek(c) >= '0' && lex_peek(c) <= '9') || lex_peek(c) == '_')) {
                if (rn + 1 >= sizeof(raw)) return lex_error(vm, c, "numeric literal too long");
                raw[rn++] = lex_peek(c);
                lex_advance(c);
            }
        }
        /* exponent part */
        if (lex_peek(c) == 'e' || lex_peek(c) == 'E') {
            is_float = true;
            if (rn + 1 >= sizeof(raw)) return lex_error(vm, c, "numeric literal too long");
            raw[rn++] = 'e';
            lex_advance(c);
            if (lex_peek(c) == '+' || lex_peek(c) == '-') {
                raw[rn++] = lex_peek(c);
                lex_advance(c);
            }
            uint32_t en = 0;
            while (!lex_end(c) && ((lex_peek(c) >= '0' && lex_peek(c) <= '9') || lex_peek(c) == '_')) {
                if (rn + 1 >= sizeof(raw)) return lex_error(vm, c, "numeric literal too long");
                raw[rn++] = lex_peek(c);
                lex_advance(c);
                en++;
            }
            if (en == 0) return lex_error(vm, c, "exponent has no digits");
        }
        raw[rn] = '\0';
    }

    uint32_t cn;
    if (!strip_underscores(cleaned, sizeof(cleaned), &cn, raw, rn)) {
        LexCursor errc = *c; errc.pos = start; errc.line = line; errc.col = col;
        return lex_error(vm, &errc, "misplaced '_' in numeric literal");
    }
    if (cn == 0) {
        LexCursor errc = *c; errc.pos = start; errc.line = line; errc.col = col;
        return lex_error(vm, &errc, "numeric literal has no digits");
    }

    if (is_float) {
        VantaResult r = token_push(vm, TK_FLOAT, line, col, start);
        if (r != VANTA_OK) return r;
        bool ok;
        double dv = v_parse_f64(cleaned, cn, &ok);
        if (!ok) return lex_error(vm, c, "malformed float literal");
        vm->tokens[vm->token_count - 1].v.f = dv;
        return VANTA_OK;
    }

    VantaResult r = token_push(vm, TK_INT, line, col, start);
    if (r != VANTA_OK) return r;
    bool ok;
    int64_t iv = v_parse_i64(cleaned, cn, base, &ok);
    if (!ok) {
        LexCursor errc;
        errc.src = vm->source; errc.len = vm->src_len;
        errc.pos = start; errc.line = line; errc.col = col;
        return lex_error(vm, &errc, "integer literal overflow");
    }
    vm->tokens[vm->token_count - 1].v.i = iv;
    return VANTA_OK;
}

/* identifier / keyword scanning */

static VantaResult lex_ident(VantaVM *vm, LexCursor *c) {
    uint32_t start = c->pos;
    uint32_t line = c->line, col = c->col;
    char buf[VANTA_NAME_MAX];
    uint32_t n = 0;

    while (!lex_end(c) && bounded_ident_char(lex_peek(c))) {
        if (n + 1 < sizeof(buf)) buf[n] = lex_peek(c);
        n++;
        lex_advance(c);
    }
    if (n >= VANTA_NAME_MAX) {
        LexCursor errc;
        errc.src = vm->source; errc.len = vm->src_len;
        errc.pos = start; errc.line = line; errc.col = col;
        return lex_error(vm, &errc, "identifier exceeds %u characters", VANTA_NAME_MAX - 1);
    }
    buf[n] = '\0';

    uint32_t kind = keyword_lookup(buf, n);
    VantaResult r = token_push(vm, kind, line, col, start);
    if (r != VANTA_OK) return r;
    if (kind == TK_IDENT) vm->tokens[vm->token_count - 1].v.len = n;
    return VANTA_OK;
}

/* string scanning
 * Escape decoding happens here; the decoded bytes are interned as a constant
 * pool entry and the token carries the index. \x is two hex digits, \u is a
 * four-hex codepoint encoded as UTF-8. Invalid escapes and unescaped control
 * bytes are syntax errors - I validate at the boundary, exactly once. */

static VantaResult lex_string(VantaVM *vm, LexCursor *c) {
    char quote = lex_peek(c);
    uint32_t start = c->pos;
    uint32_t line = c->line, col = c->col;

    lex_advance(c);                       /* opening quote */

    VantaSB *sb = &vm->sb;
    sb_reset(sb);

    bool closed = false;
    while (!lex_end(c)) {
        char ch = lex_peek(c);
        if (ch == quote) { lex_advance(c); closed = true; break; }
        if (ch == '\n') {
            LexCursor errc;
            errc.src = vm->source; errc.len = vm->src_len;
            errc.pos = start; errc.line = line; errc.col = col;
            return lex_error(vm, &errc, "unterminated string literal (newline inside)");
        }
        if (ch == '\\') {
            lex_advance(c);
            char e = lex_peek(c);
            lex_advance(c);
            switch (e) {
                case 'n':  if (!sb_append_char(sb, '\n')) return VANTA_ERR_OUT_OF_MEMORY; break;
                case 't':  if (!sb_append_char(sb, '\t')) return VANTA_ERR_OUT_OF_MEMORY; break;
                case 'r':  if (!sb_append_char(sb, '\r')) return VANTA_ERR_OUT_OF_MEMORY; break;
                case '\\': if (!sb_append_char(sb, '\\')) return VANTA_ERR_OUT_OF_MEMORY; break;
                case '\'': if (!sb_append_char(sb, '\'')) return VANTA_ERR_OUT_OF_MEMORY; break;
                case '"':  if (!sb_append_char(sb, '"'))  return VANTA_ERR_OUT_OF_MEMORY; break;
                case '0':  if (!sb_append_char(sb, '\0')) return VANTA_ERR_OUT_OF_MEMORY; break;
                case 'x': {
                    int h1 = hex_digit_val(lex_peek(c));
                    if (h1 < 0) {
                        return lex_error(vm, c, "\\x escape needs two hex digits");
                    }
                    lex_advance(c);
                    int h2 = hex_digit_val(lex_peek(c));
                    if (h2 < 0) {
                        return lex_error(vm, c, "\\x escape needs two hex digits");
                    }
                    lex_advance(c);
                    if (!sb_append_char(sb, (char)((h1 << 4) | h2))) return VANTA_ERR_OUT_OF_MEMORY;
                    break;
                }
                case 'u': {
                    uint32_t cp = 0;
                    for (int i = 0; i < 4; i++) {
                        int h = hex_digit_val(lex_peek(c));
                        if (h < 0) return lex_error(vm, c, "\\u escape needs four hex digits");
                        cp = cp * 16 + (uint32_t)h;
                        lex_advance(c);
                    }
                    if (cp >= 0xD800 && cp <= 0xDFFF) {
                        return lex_error(vm, c, "\\u escape is a surrogate (invalid)");
                    }
                    char enc[4];
                    uint32_t en = utf8_encode(cp, enc);
                    if (en == 0) return lex_error(vm, c, "\\u escape out of Unicode range");
                    if (!sb_append(sb, enc, en)) return VANTA_ERR_OUT_OF_MEMORY;
                    break;
                }
                default:
                    return lex_error(vm, c, "unknown escape sequence '\\%c'", e);
            }
            continue;
        }
        if (!sb_append_char(sb, ch)) return VANTA_ERR_OUT_OF_MEMORY;
        lex_advance(c);
    }

    if (!closed) {
        LexCursor errc;
        errc.src = vm->source; errc.len = vm->src_len;
        errc.pos = start; errc.line = line; errc.col = col;
        return lex_error(vm, &errc, "unterminated string literal");
    }

    /* intern into the constant table (dedup below in add_const) */
    uint32_t cidx;
    VantaResult cr = add_const_string(vm, sb->space, sb->len, &cidx);
    if (cr != VANTA_OK) return cr;

    VantaResult r = token_push(vm, TK_STR, line, col, start);
    if (r != VANTA_OK) return r;
    vm->tokens[vm->token_count - 1].v.len = cidx;
    sb_reset(sb);
    return VANTA_OK;
}

/* operator scanning */

static VantaResult lex_op(VantaVM *vm, LexCursor *c) {
    uint32_t line = c->line, col = c->col;
    uint32_t start = c->pos;
    char c0 = lex_peek(c);
    char c1 = lex_ahead(c, 1);
    char c2 = lex_ahead(c, 2);

    uint32_t kind = TK_EOF;
    uint32_t consume = 1;

    switch (c0) {
        case '(': kind = TK_LPAREN; break;
        case ')': kind = TK_RPAREN; break;
        case '{': kind = TK_LBRACE; break;
        case '}': kind = TK_RBRACE; break;
        case '[': kind = TK_LBRACKET; break;
        case ']': kind = TK_RBRACKET; break;
        case ',': kind = TK_COMMA; break;
        case ';': kind = TK_SEMI; break;
        case '?': kind = TK_QUESTION; break;
        case '~': kind = TK_TILDE; break;
        case '.': kind = TK_DOT; break;
        case ':': kind = TK_COLON; break;
        case '+':
            if (c1 == '+')      { kind = TK_PLUSPLUS;    consume = 2; }
            else if (c1 == '=') { kind = TK_PLUS_ASSIGN; consume = 2; }
            else                  kind = TK_PLUS;
            break;
        case '-':
            if (c1 == '-')      { kind = TK_MINUSMINUS;   consume = 2; }
            else if (c1 == '=') { kind = TK_MINUS_ASSIGN; consume = 2; }
            else                  kind = TK_MINUS;
            break;
        case '*':
            if (c1 == '=')      { kind = TK_STAR_ASSIGN; consume = 2; }
            else                  kind = TK_STAR;
            break;
        case '/':
            if (c1 == '=')      { kind = TK_SLASH_ASSIGN; consume = 2; }
            else                  kind = TK_SLASH;
            break;
        case '%':
            if (c1 == '=')      { kind = TK_PERCENT_ASSIGN; consume = 2; }
            else                  kind = TK_PERCENT;
            break;
        case '&':
            if (c1 == '&')          { kind = TK_AND;         consume = 2; }
            else if (c1 == '=')     { kind = TK_BAND_ASSIGN; consume = 2; }
            else                      kind = TK_AMP;
            break;
        case '|':
            if (c1 == '|')          { kind = TK_OR;         consume = 2; }
            else if (c1 == '=')     { kind = TK_BOR_ASSIGN; consume = 2; }
            else                      kind = TK_PIPE;
            break;
        case '^':
            if (c1 == '=')      { kind = TK_BXOR_ASSIGN; consume = 2; }
            else                  kind = TK_CARET;
            break;
        case '!':
            if (c1 == '=')      { kind = TK_NE; consume = 2; }
            else                  kind = TK_BANG;
            break;
        case '=':
            if (c1 == '=')      { kind = TK_EQ; consume = 2; }
            else                  kind = TK_ASSIGN;
            break;
        case '<':
            if (c1 == '=')          { kind = TK_LE;        consume = 2; }
            else if (c1 == '<') {
                if (c2 == '=')      { kind = TK_SHL_ASSIGN; consume = 3; }
                else                  { kind = TK_SHL;        consume = 2; }
            }
            else                      kind = TK_LT;
            break;
        case '>':
            if (c1 == '=')          { kind = TK_GE;        consume = 2; }
            else if (c1 == '>') {
                if (c2 == '=')      { kind = TK_SHR_ASSIGN; consume = 3; }
                else                  { kind = TK_SHR;        consume = 2; }
            }
            else                      kind = TK_GT;
            break;
        default:
            break;
    }

    if (kind == TK_EOF) {
        /* printable diagnostic even for high bytes */
        char shown = (c0 >= 0x20 && c0 < 0x7F) ? c0 : '?';
        return lex_error(vm, c, "unexpected character '%c' (0x%02x)", shown, (unsigned char)c0);
    }

    VantaResult r = token_push(vm, kind, line, col, start);
    if (r != VANTA_OK) return r;
    for (uint32_t i = 0; i < consume; i++) lex_advance(c);
    return VANTA_OK;
}

/* comment handling */

/* Skip whitespace and comments; returns false on unterminated block comment
 * (diag filled). Newlines are NOT consumed here - they are significant. */
static VantaResult lex_skip_junk(VantaVM *vm, LexCursor *c) {
    while (!lex_end(c)) {
        char ch = lex_peek(c);
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\v' || ch == '\f') {
            lex_advance(c);
            continue;
        }
        if (ch == '/' && c->pos + 1 < c->len) {
            if (c->src[c->pos + 1] == '/') {
                /* line comment: to end of line (newline token emitted separately) */
                while (!lex_end(c) && lex_peek(c) != '\n') lex_advance(c);
                continue;
            }
            if (c->src[c->pos + 1] == '*') {
                uint32_t bline = c->line, bcol = c->col, bpos = c->pos;
                lex_advance(c);
                lex_advance(c);
                bool closed_comment = false;
                while (!lex_end(c)) {
                    if (lex_peek(c) == '*' && lex_ahead(c, 1) == '/') {
                        lex_advance(c);
                        lex_advance(c);
                        closed_comment = true;
                        break;
                    }
                    lex_advance(c);
                }
                if (!closed_comment) {
                    LexCursor errc;
                    errc.src = vm->source; errc.len = vm->src_len;
                    errc.pos = bpos; errc.line = bline; errc.col = bcol;
                    return lex_error(vm, &errc, "unterminated block comment");
                }
                continue;
            }
        }
        break;
    }
    return VANTA_OK;
}

/* driver */

static VantaResult lex_program(VantaVM *vm) {
    LexCursor c;
    c.src = vm->source;
    c.len = vm->src_len;
    c.pos = 0;
    c.line = 1;
    c.col = 1;

    vm->token_count = 0;

    while (true) {
        VantaResult r = lex_skip_junk(vm, &c);
        if (r != VANTA_OK) return r;
        if (lex_end(&c)) break;

        char ch = lex_peek(&c);

        if (ch == '\n') {
            uint32_t lk = last_significant_kind(vm);
            lex_advance(&c);
            if (lex_suppress_nl_after(lk)) continue;   /* continuation, not terminator */
            if (lk == TK_NL) continue;                 /* collapse blank runs */
            r = token_push(vm, TK_NL, c.line - 1, c.col, c.pos - 1);
            if (r != VANTA_OK) return r;
            continue;
        }

        if ((ch >= '0' && ch <= '9') ||
            (ch == '.' && (lex_ahead(&c, 1) >= '0' && lex_ahead(&c, 1) <= '9'))) {
            r = lex_number(vm, &c);
            if (r != VANTA_OK) return r;
            continue;
        }

        if (ident_start_char(ch)) {
            r = lex_ident(vm, &c);
            if (r != VANTA_OK) return r;
            continue;
        }

        if (ch == '"' || ch == '\'') {
            r = lex_string(vm, &c);
            if (r != VANTA_OK) return r;
            continue;
        }

        r = lex_op(vm, &c);
        if (r != VANTA_OK) return r;
    }

    /* file ends as a statement terminator too */
    uint32_t lk = last_significant_kind(vm);
    if (lk != TK_EOF && lk != TK_NL && !lex_suppress_nl_after(lk)) {
        VantaResult r = token_push(vm, TK_NL, c.line, c.col, c.pos);
        if (r != VANTA_OK) return r;
    }

    return token_push(vm, TK_EOF, c.line, c.col, c.pos);
}

/* P13 - compiler infrastructure
 *
 * Single-pass recursive descent from tokens to bytecode. Names, globals,
 * locals and function records live in bounded VM tables. Every emit checks
 * the bytecode arena bound; every declaration checks its table bound.
 * Scope exit REUSES freed local slots (scope depth watermark on the local
 * descriptor stack), so deep block scopes stay flat on slot usage. */

#define GLOBAL_FLAG_CONST    1u
#define GLOBAL_FLAG_DEFINED  2u

static VantaResult comp_error_at(VantaVM *vm, uint32_t line, uint32_t col,
                                 const char *fmt, ...) {
    char msg[VANTA_DIAG_MSG];
    va_list ap;
    va_start(ap, fmt);
    int n = vanta_vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;

    VantaDiag *d = &vm->diag;
    d->result = VANTA_ERR_SYNTAX;
    d->line = line;
    d->col = col;
    d->msg_len = (uint32_t)v_strlen(msg);
    v_strlcpy(d->msg, msg, sizeof(d->msg));
    return VANTA_ERR_SYNTAX;
}

static VantaResult comp_error_tok(VantaVM *vm, const VantaToken *t, const char *fmt, ...) {
    char msg[VANTA_DIAG_MSG];
    va_list ap;
    va_start(ap, fmt);
    int n = vanta_vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;

    VantaDiag *d = &vm->diag;
    d->result = VANTA_ERR_SYNTAX;
    d->line = t->line;
    d->col = t->col;
    d->msg_len = (uint32_t)v_strlen(msg);
    v_strlcpy(d->msg, msg, sizeof(d->msg));
    return VANTA_ERR_SYNTAX;
}

/* name interning */

static int32_t name_index(VantaVM *vm, const char *s, uint32_t len) {
    if (len >= VANTA_NAME_MAX) len = VANTA_NAME_MAX - 1;
    for (uint32_t i = 0; i < vm->name_count; i++) {
        if (vm->names[i].len == len && v_memcmp_eq(vm->names[i].name, s, len)) {
            return (int32_t)i;
        }
    }
    if (vm->name_count >= VANTA_MAX_NAMES) return -1;
    uint32_t idx = vm->name_count++;
    v_memcpy(vm->names[idx].name, s, len);
    vm->names[idx].name[len] = '\0';
    vm->names[idx].len = len;
    return (int32_t)idx;
}

/* bytecode emission */

static uint32_t bc_pos(const VantaVM *vm) { return vm->bc_len; }

static VantaResult emit_u8(VantaVM *vm, uint8_t b) {
    if (vm->bc_len + 1 > VANTA_MAX_BYTECODE) {
        return comp_error_at(vm, 0, 0, "bytecode limit (%u bytes) exceeded", VANTA_MAX_BYTECODE);
    }
    vm->bytecode[vm->bc_len++] = b;
    return VANTA_OK;
}

static VantaResult emit_u32(VantaVM *vm, uint32_t v) {
    if (vm->bc_len + 4 > VANTA_MAX_BYTECODE) {
        return comp_error_at(vm, 0, 0, "bytecode limit (%u bytes) exceeded", VANTA_MAX_BYTECODE);
    }
    vm->bytecode[vm->bc_len++] = (uint8_t)(v & 0xFF);
    vm->bytecode[vm->bc_len++] = (uint8_t)((v >> 8) & 0xFF);
    vm->bytecode[vm->bc_len++] = (uint8_t)((v >> 16) & 0xFF);
    vm->bytecode[vm->bc_len++] = (uint8_t)((v >> 24) & 0xFF);
    return VANTA_OK;
}

static VantaResult emit_i64(VantaVM *vm, int64_t v) {
    for (int i = 0; i < 8; i++) {
        VantaResult r = emit_u8(vm, (uint8_t)((uint64_t)v >> (i * 8)));
        if (r != VANTA_OK) return r;
    }
    return VANTA_OK;
}

static VantaResult emit_f64(VantaVM *vm, double v) {
    uint64_t bits = f64_to_bits(v);
    for (int i = 0; i < 8; i++) {
        VantaResult r = emit_u8(vm, (uint8_t)(bits >> (i * 8)));
        if (r != VANTA_OK) return r;
    }
    return VANTA_OK;
}

static VantaResult emit_op(VantaVM *vm, uint32_t op) {
    if (op >= OP_COUNT) return VANTA_ERR_INTERNAL;
    return emit_u8(vm, (uint8_t)op);
}

static VantaResult emit_op_u32(VantaVM *vm, uint32_t op, uint32_t arg) {
    VantaResult r = emit_op(vm, op);
    if (r != VANTA_OK) return r;
    return emit_u32(vm, arg);
}

/* Jump placeholder: emits op + NO_INDEX, returns the operand position for
 * a later patch_jump. Patch target validation happens at patch time. */
static VantaResult emit_jump(VantaVM *vm, uint32_t op, uint32_t *patch_pos) {
    VantaResult r = emit_op(vm, op);
    if (r != VANTA_OK) return r;
    *patch_pos = bc_pos(vm);
    return emit_u32(vm, VANTA_NO_INDEX);
}

static void patch_jump(VantaVM *vm, uint32_t patch_pos, uint32_t target) {
    if (patch_pos == VANTA_NO_INDEX || patch_pos + 4 > vm->bc_len) return;
    if (target > vm->bc_len) target = vm->bc_len;
    vm->bytecode[patch_pos]     = (uint8_t)(target & 0xFF);
    vm->bytecode[patch_pos + 1] = (uint8_t)((target >> 8) & 0xFF);
    vm->bytecode[patch_pos + 2] = (uint8_t)((target >> 16) & 0xFF);
    vm->bytecode[patch_pos + 3] = (uint8_t)((target >> 24) & 0xFF);
}

/* Patch lists thread through emitted placeholders: the placeholder holds the
 * offset of the next patch in the chain, NO_INDEX at the tail. Break and
 * continue lists use the same machinery. */
static VantaResult patch_list_add(VantaVM *vm, uint32_t op, uint32_t *head) {
    VantaResult r = emit_op(vm, op);
    if (r != VANTA_OK) return r;
    uint32_t pos = bc_pos(vm);
    r = emit_u32(vm, *head);
    if (r != VANTA_OK) return r;
    *head = pos;
    return VANTA_OK;
}

static void patch_list_resolve(VantaVM *vm, uint32_t head, uint32_t target) {
    uint32_t cur = head;
    while (cur != VANTA_NO_INDEX) {
        if (cur + 4 > vm->bc_len) break;
        uint32_t next = (uint32_t)vm->bytecode[cur]
                      | ((uint32_t)vm->bytecode[cur + 1] << 8)
                      | ((uint32_t)vm->bytecode[cur + 2] << 16)
                      | ((uint32_t)vm->bytecode[cur + 3] << 24);
        patch_jump(vm, cur, target);
        cur = next;
    }
}

/* line map
 * One entry per source line that contributes code. The runtime maps pc ->
 * line via binary search for stack traces; the disassembler prints source
 * lines next to opcodes. Entries stop recording once the table is full -
 * diagnostics degrade (later lines report the last map entry) but never
 * corrupt the bytecode stream. */

static void note_source_line(VantaVM *vm, uint32_t line) {
    if (line == 0 || line == vm->last_line_emitted) return;
    if (vm->lineinfo_count >= VANTA_MAX_LINEINFO) return;
    vm->lineinfo[vm->lineinfo_count].pc = vm->bc_len;
    vm->lineinfo[vm->lineinfo_count].line = line;
    vm->lineinfo_count++;
    vm->last_line_emitted = line;
}

static uint32_t pc_to_line(VantaVM *vm, uint32_t pc) {
    if (vm->lineinfo_count == 0) return 0;
    uint32_t lo = 0, hi = vm->lineinfo_count;
    while (lo + 1 < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (vm->lineinfo[mid].pc <= pc) lo = mid;
        else hi = mid;
    }
    return vm->lineinfo[lo].line;
}

/* constant strings */

static VantaResult add_const_string(VantaVM *vm, const char *bytes, uint32_t len,
                                    uint32_t *idx_out) {
    *idx_out = VANTA_NO_INDEX;

    /* dedup: identical literal bytes share one constant slot */
    for (uint32_t i = 0; i < vm->const_count; i++) {
        if (vm->consts[i].len == len &&
            v_memcmp_eq(vm->const_data + vm->consts[i].off, bytes, len)) {
            *idx_out = i;
            return VANTA_OK;
        }
    }

    if (vm->const_count >= VANTA_MAX_CONSTS) {
        return comp_error_at(vm, 0, 0, "string constant limit (%u) exceeded", VANTA_MAX_CONSTS);
    }
    if (vm->const_data_len + len + 1 > VANTA_MAX_CONST_DATA) {
        return comp_error_at(vm, 0, 0, "string constant storage limit (%u bytes) exceeded",
                             VANTA_MAX_CONST_DATA);
    }

    uint32_t idx = vm->const_count++;
    vm->consts[idx].off = vm->const_data_len;
    vm->consts[idx].len = len;
    if (len) v_memcpy(vm->const_data + vm->const_data_len, bytes, len);
    vm->const_data_len += len;
    vm->const_data[vm->const_data_len++] = '\0';
    *idx_out = idx;
    return VANTA_OK;
}

/* globals */

static int32_t global_find(VantaVM *vm, uint32_t name_idx) {
    for (uint32_t i = 0; i < vm->global_count; i++) {
        if (vm->globals[i].name_idx == name_idx) return (int32_t)i;
    }
    return -1;
}

/* declare-or-get. `flags` seeds a fresh slot; existing slots keep theirs. */
static int32_t global_declare(VantaVM *vm, uint32_t name_idx, uint32_t flags) {
    int32_t slot = global_find(vm, name_idx);
    if (slot >= 0) return slot;
    if (vm->global_count >= VANTA_MAX_GLOBALS) return -1;
    uint32_t idx = vm->global_count++;
    vm->globals[idx].name_idx = name_idx;
    vm->globals[idx].value = VANTA_VALUE_NIL;
    vm->globals[idx].flags = flags;
    vm->globals[idx].pad = 0;
    return (int32_t)idx;
}

static const char *global_name_str(VantaVM *vm, uint32_t slot) {
    uint32_t ni = vm->globals[slot].name_idx;
    return vm->names[ni].name;
}

/* locals and scopes
 * vm->cur_fn owns the current local slot space. Entering a function body
 * snapshots the compiler state into a C-local struct (recursive descent
 * nests naturally on the C stack); leaving restores it. Within a function,
 * scope_depth tracks { }; exit_scope pops descriptors down to the entry
 * watermark and releases their slots for reuse. */

typedef struct {
    uint32_t local_count;
    uint32_t localdesc_count;
    uint32_t scope_depth;
    uint32_t loop_depth;
    uint32_t cur_fn;
} CompilerFnState;

static void fn_state_save(VantaVM *vm, CompilerFnState *st) {
    st->local_count = vm->local_count;
    st->localdesc_count = vm->localdesc_count;
    st->scope_depth = vm->scope_depth;
    st->loop_depth = vm->loop_depth;
    st->cur_fn = vm->cur_fn;
}

static void fn_state_restore(VantaVM *vm, const CompilerFnState *st) {
    vm->local_count = st->local_count;
    vm->localdesc_count = st->localdesc_count;
    vm->scope_depth = st->scope_depth;
    vm->loop_depth = st->loop_depth;
    vm->cur_fn = st->cur_fn;
}

/* visible descriptor for a name in the current function at current depth */
static int32_t local_find(VantaVM *vm, uint32_t name_idx, uint32_t *slot_out,
                          bool *is_const) {
    for (uint32_t i = vm->localdesc_count; i-- > 0;) {
        VantaLocalDesc *d = &vm->localdesc[i];
        if (d->fn_idx != vm->cur_fn) continue;
        if (d->depth > vm->scope_depth) continue;      /* out of scope already */
        if (d->name_idx == name_idx) {
            if (slot_out) *slot_out = d->slot;
            if (is_const) *is_const = d->is_const != 0;
            return (int32_t)i;
        }
    }
    return -1;
}

static VantaResult local_declare(VantaVM *vm, uint32_t name_idx, bool is_const,
                                 uint32_t line, uint32_t col, uint32_t *slot_out) {
    /* shadowing the same name in one scope is a bug magnet: reject it */
    for (uint32_t i = vm->localdesc_count; i-- > 0;) {
        VantaLocalDesc *d = &vm->localdesc[i];
        if (d->fn_idx != vm->cur_fn || d->depth != vm->scope_depth) continue;
        if (d->name_idx == name_idx) {
            return comp_error_at(vm, line, col, "identifier '%s' already declared in this scope",
                                 vm->names[name_idx].name);
        }
    }

    if (vm->localdesc_count >= VANTA_MAX_LOCALDESC) {
        return comp_error_at(vm, line, col, "local declaration limit (%u) exceeded",
                             VANTA_MAX_LOCALDESC);
    }
    if (vm->local_count >= VANTA_MAX_LOCALS) {
        return comp_error_at(vm, line, col, "local slot limit (%u) exceeded", VANTA_MAX_LOCALS);
    }

    uint32_t slot = vm->local_count++;
    VantaLocalDesc *d = &vm->localdesc[vm->localdesc_count++];
    d->name_idx = name_idx;
    d->slot = slot;
    d->depth = vm->scope_depth;
    d->fn_idx = vm->cur_fn;
    d->is_const = is_const;
    d->pad = 0;
    if (slot_out) *slot_out = slot;
    return VANTA_OK;
}

/* mark a parameter slot; parameters live at depth 0 of the function */
static VantaResult local_declare_param(VantaVM *vm, uint32_t name_idx, uint32_t *slot_out) {
    return local_declare(vm, name_idx, false, 0, 0, slot_out);
}

static void enter_scope(VantaVM *vm) {
    if (vm->scope_depth < VANTA_MAX_BLOCK_DEPTH) vm->scope_depth++;
}

/* pop descriptors deeper than the watermark; slots become reusable. The
 * runtime slots themselves are simply overwritten by later locals - no
 * cleanup needed since nothing above them is live after scope exit. */
static void exit_scope(VantaVM *vm, uint32_t entry_depth) {
    while (vm->localdesc_count > 0) {
        VantaLocalDesc *d = &vm->localdesc[vm->localdesc_count - 1];
        if (d->fn_idx != vm->cur_fn || d->depth <= entry_depth) break;
        vm->localdesc_count--;
    }
    vm->scope_depth = entry_depth;
}

/* loop stack */

static VantaResult push_loop(VantaVM *vm, uint32_t fn_idx) {
    if (vm->loop_depth >= VANTA_MAX_LOOP_DEPTH) {
        return comp_error_at(vm, 0, 0, "loop nesting limit (%u) exceeded", VANTA_MAX_LOOP_DEPTH);
    }
    VantaLoop *l = &vm->loops[vm->loop_depth++];
    l->break_patches = VANTA_NO_INDEX;
    l->continue_patches = VANTA_NO_INDEX;
    l->continue_pc = VANTA_NO_INDEX;
    l->drop_slots = 0;
    l->loop_depth = vm->scope_depth;
    l->fn_idx = fn_idx;
    return VANTA_OK;
}

static VantaLoop *cur_loop(VantaVM *vm) {
    if (vm->loop_depth == 0) return NULL;
    return &vm->loops[vm->loop_depth - 1];
}

static void pop_loop(VantaVM *vm) {
    if (vm->loop_depth) vm->loop_depth--;
}

/* identifier resolution
 * Order: local -> global -> builtin. The parser asks for all three at once
 * and decides per result; the same lookup is used by assignment targets. */

typedef enum {
    RESOLVE_NONE = 0,
    RESOLVE_LOCAL,
    RESOLVE_GLOBAL,
    RESOLVE_BUILTIN,
    RESOLVE_METHOD
} ResolveKind;

typedef struct {
    ResolveKind kind;
    uint32_t    idx;        /* slot / global slot / native id */
    uint32_t    slot;       /* local slot when kind == RESOLVE_LOCAL */
    bool        is_const;
    bool        is_defined; /* global: false until first assignment runs */
} ResolveOut;

static int32_t builtin_lookup_id(VantaVM *vm, const char *s, uint32_t len);

static bool resolve_ident(VantaVM *vm, uint32_t name_idx, ResolveOut *out) {
    out->kind = RESOLVE_NONE;
    out->idx = 0;
    out->slot = 0;
    out->is_const = false;
    out->is_defined = true;

    uint32_t slot;
    bool is_const;
    if (local_find(vm, name_idx, &slot, &is_const) >= 0) {
        out->kind = RESOLVE_LOCAL;
        out->slot = slot;
        out->is_const = is_const;
        return true;
    }

    int32_t g = global_find(vm, name_idx);
    if (g >= 0) {
        out->kind = RESOLVE_GLOBAL;
        out->idx = (uint32_t)g;
        out->is_defined = (vm->globals[g].flags & GLOBAL_FLAG_DEFINED) != 0;
        out->is_const = (vm->globals[g].flags & GLOBAL_FLAG_CONST) != 0;
        return true;
    }

    int32_t nat = builtin_lookup_id(vm, vm->names[name_idx].name, vm->names[name_idx].len);
    if (nat >= 0) {
        out->kind = RESOLVE_BUILTIN;
        out->idx = (uint32_t)nat;
        return true;
    }
    return false;
}

/* forward declarations for the recursive descent */

static VantaResult compile_expr_prec(VantaVM *vm, uint32_t min_prec);
static VantaResult compile_statement(VantaVM *vm, bool top_level);
static VantaResult compile_block(VantaVM *vm);
static VantaResult compile_call_args(VantaVM *vm, uint32_t *argc_out);

/* token cursor helpers used everywhere in P14/P15 */

static const VantaToken *tok_cur(VantaVM *vm) {
    return &vm->tokens[vm->cur_token < vm->token_count ? vm->cur_token : vm->token_count - 1];
}

static void tok_advance(VantaVM *vm) {
    if (vm->cur_token < vm->token_count) vm->cur_token++;
}

static bool tok_is(VantaVM *vm, uint32_t kind) {
    return tok_cur(vm)->kind == kind;
}

static void skip_nl(VantaVM *vm) {
    while (tok_is(vm, TK_NL)) tok_advance(vm);
}

static VantaResult expect_tok(VantaVM *vm, uint32_t kind) {
    if (tok_is(vm, kind)) { tok_advance(vm); return VANTA_OK; }
    const VantaToken *t = tok_cur(vm);
    return comp_error_tok(vm, t, "expected %s, got %s",
                          token_kind_name(kind), token_kind_name(t->kind));
}

/* consume a statement terminator: ';' or newline. Blocks handle their own
 * boundaries. Missing terminator is an error - no silent statement gluing. */
static VantaResult expect_terminator(VantaVM *vm) {
    if (tok_is(vm, TK_NL)) {
        skip_nl(vm);
        return VANTA_OK;
    }
    if (tok_is(vm, TK_SEMI)) {
        tok_advance(vm);
        return VANTA_OK;
    }
    if (tok_is(vm, TK_RBRACE) || tok_is(vm, TK_EOF)) return VANTA_OK;
    const VantaToken *t = tok_cur(vm);
    return comp_error_tok(vm, t, "expected end of statement, got %s",
                          token_kind_name(t->kind));
}

/* P14 - expression compiler: precedence climbing over the token stream
 *
 * Grammar (lowest to highest precedence):
 *   ternary      cond ? a : b            right-assoc
 *   logical      ||  &&                  short-circuit, result is bool
 *   bitwise      |  ^  &
 *   equality     ==  !=
 *   relational   <  <=  >  >=
 *   shift        <<  >>
 *   additive     +  -                    '+' doubles as string/array concat
 *   multiplicative  *  /  %
 *   unary        - ! ~ ++x --x
 *   postfix      call  [index]  .member  x++  x--
 *   primary      literals, names, (e), [a,b], {k:v}, fn
 *
 * Everything emits straight to bytecode; jump targets are patched through
 * placeholder chains from P13. No AST is ever built - compile and parse are
 * one walk, which keeps the working set to the token cursor plus a handful
 * of patch positions. */

#define PREC_TERNARY 1u
#define PREC_OR      2u
#define PREC_AND     3u
#define PREC_BITOR   4u
#define PREC_BITXOR  5u
#define PREC_BITAND  6u
#define PREC_EQUAL   7u
#define PREC_REL     8u
#define PREC_SHIFT   9u
#define PREC_ADD    10u
#define PREC_MUL    11u

typedef struct {
    uint32_t kind;      /* token kind of the operator */
    uint32_t prec;
    uint32_t op;        /* OP_* to emit, or NO_INDEX for special forms */
} BinOpInfo;

static bool binop_info(uint32_t kind, BinOpInfo *out) {
    switch (kind) {
        case TK_OR:      out->kind = kind; out->prec = PREC_OR;     out->op = VANTA_NO_INDEX; return true;
        case TK_AND:     out->kind = kind; out->prec = PREC_AND;    out->op = VANTA_NO_INDEX; return true;
        case TK_PIPE:    out->kind = kind; out->prec = PREC_BITOR;  out->op = OP_BOR;  return true;
        case TK_CARET:   out->kind = kind; out->prec = PREC_BITXOR; out->op = OP_BXOR; return true;
        case TK_AMP:     out->kind = kind; out->prec = PREC_BITAND; out->op = OP_BAND; return true;
        case TK_EQ:      out->kind = kind; out->prec = PREC_EQUAL;  out->op = OP_EQ;   return true;
        case TK_NE:      out->kind = kind; out->prec = PREC_EQUAL;  out->op = OP_NE;   return true;
        case TK_LT:      out->kind = kind; out->prec = PREC_REL;    out->op = OP_LT;   return true;
        case TK_LE:      out->kind = kind; out->prec = PREC_REL;    out->op = OP_LE;   return true;
        case TK_GT:      out->kind = kind; out->prec = PREC_REL;    out->op = OP_GT;   return true;
        case TK_GE:      out->kind = kind; out->prec = PREC_REL;    out->op = OP_GE;   return true;
        case TK_SHL:     out->kind = kind; out->prec = PREC_SHIFT;  out->op = OP_SHL;  return true;
        case TK_SHR:     out->kind = kind; out->prec = PREC_SHIFT;  out->op = OP_SHR;  return true;
        case TK_PLUS:    out->kind = kind; out->prec = PREC_ADD;    out->op = OP_ADD;  return true;
        case TK_MINUS:   out->kind = kind; out->prec = PREC_ADD;    out->op = OP_SUB;  return true;
        case TK_STAR:    out->kind = kind; out->prec = PREC_MUL;    out->op = OP_MUL;  return true;
        case TK_SLASH:   out->kind = kind; out->prec = PREC_MUL;    out->op = OP_DIV;  return true;
        case TK_PERCENT: out->kind = kind; out->prec = PREC_MUL;    out->op = OP_MOD;  return true;
        default: return false;
    }
}

/* The postfix chain reports whether the FINAL expression is still a plain
 * variable reference, so prefix/postfix ++ can target it directly. */
typedef struct {
    bool     is_var;
    bool     is_local;
    bool     is_const;
    uint32_t slot;         /* local slot or global slot */
    uint32_t name_idx;
} PrimaryInfo;

static VantaResult push_native_value(VantaVM *vm, uint32_t native_id);
static VantaResult compile_fn_expression(VantaVM *vm, const VantaToken *fn_tok,
                                         PrimaryInfo *pi);
static VantaResult compile_postfix_loop(VantaVM *vm, PrimaryInfo *pi);

/* primary */

static VantaResult compile_primary(VantaVM *vm, PrimaryInfo *pi) {
    pi->is_var = false;
    pi->is_local = false;
    pi->is_const = false;
    pi->slot = 0;
    pi->name_idx = VANTA_NO_INDEX;

    skip_nl(vm);
    const VantaToken *t = tok_cur(vm);
    note_source_line(vm, t->line);

    switch (t->kind) {
        case TK_INT:
            tok_advance(vm);
            {
                VantaResult r = emit_op(vm, OP_PUSH_INT);
                if (r) return r;
                return emit_i64(vm, t->v.i);
            }

        case TK_FLOAT:
            tok_advance(vm);
            {
                VantaResult r = emit_op(vm, OP_PUSH_FLOAT);
                if (r) return r;
                return emit_f64(vm, t->v.f);
            }

        case TK_STR:
            tok_advance(vm);
            return emit_op_u32(vm, OP_PUSH_CONST, t->v.len);

        case TK_K_TRUE:
            tok_advance(vm);
            return emit_op(vm, OP_PUSH_TRUE);

        case TK_K_FALSE:
            tok_advance(vm);
            return emit_op(vm, OP_PUSH_FALSE);

        case TK_K_NIL:
            tok_advance(vm);
            return emit_op(vm, OP_PUSH_NIL);

        case TK_LPAREN: {
            tok_advance(vm);
            VantaResult r = compile_expr_prec(vm, PREC_TERNARY);
            if (r) return r;
            skip_nl(vm);
            return expect_tok(vm, TK_RPAREN);
        }

        case TK_LBRACKET: {
            /* array literal: elements compiled in order, ARRAY_NEW collects */
            tok_advance(vm);
            uint32_t count = 0;
            skip_nl(vm);
            while (!tok_is(vm, TK_RBRACKET)) {
                if (count >= VANTA_ARR_MAX) {
                    return comp_error_tok(vm, tok_cur(vm),
                                          "array literal exceeds %u elements", VANTA_ARR_MAX);
                }
                VantaResult r = compile_expr_prec(vm, PREC_TERNARY);
                if (r) return r;
                count++;
                skip_nl(vm);
                if (tok_is(vm, TK_COMMA)) { tok_advance(vm); skip_nl(vm); continue; }
                break;
            }
            if (!tok_is(vm, TK_RBRACKET)) {
                return comp_error_tok(vm, tok_cur(vm), "expected ']' to close array literal");
            }
            tok_advance(vm);
            return emit_op_u32(vm, OP_ARRAY_NEW, count);
        }

        case TK_LBRACE: {
            /* map literal at EXPRESSION position. Statement position never
             * routes here (blocks are parsed by the statement compiler). */
            tok_advance(vm);
            uint32_t entries = 0;
            skip_nl(vm);
            while (!tok_is(vm, TK_RBRACE)) {
                /* key: string literal or bare identifier (sugar) */
                uint32_t key_const = VANTA_NO_INDEX;
                if (tok_is(vm, TK_STR)) {
                    key_const = tok_cur(vm)->v.len;
                    tok_advance(vm);
                } else if (tok_is(vm, TK_IDENT)) {
                    const VantaToken *kt = tok_cur(vm);
                    VantaResult r = add_const_string(vm, vm->source + kt->src_off, kt->v.len,
                                                     &key_const);
                    if (r) return r;
                    tok_advance(vm);
                } else {
                    return comp_error_tok(vm, tok_cur(vm),
                                          "map key must be a string or identifier, got %s",
                                          token_kind_name(tok_cur(vm)->kind));
                }
                VantaResult r = expect_tok(vm, TK_COLON);
                if (r) return r;
                skip_nl(vm);
                r = compile_expr_prec(vm, PREC_TERNARY);
                if (r) return r;
                /* MAP_NEW takes (key, value) pairs bottom-up: the value is
                 * already compiled, so push the key and swap into order */
                r = emit_op_u32(vm, OP_PUSH_CONST, key_const);
                if (r) return r;
                r = emit_op(vm, OP_SWAP);
                if (r) return r;
                entries++;
                skip_nl(vm);
                if (tok_is(vm, TK_COMMA)) { tok_advance(vm); skip_nl(vm); continue; }
                break;
            }
            if (!tok_is(vm, TK_RBRACE)) {
                return comp_error_tok(vm, tok_cur(vm), "expected '}' to close map literal");
            }
            tok_advance(vm);
            return emit_op_u32(vm, OP_MAP_NEW, entries);
        }

        case TK_K_FN:
            /* function expression: fn (a, b) { ... } or fn name(a) { ... } */
            return compile_fn_expression(vm, t, pi);

        case TK_IDENT: {
            tok_advance(vm);
            int32_t ni = name_index(vm, vm->source + t->src_off, t->v.len);
            if (ni < 0) {
                return comp_error_tok(vm, t, "identifier table exhausted");
            }
            pi->name_idx = (uint32_t)ni;

            ResolveOut res;
            if (!resolve_ident(vm, (uint32_t)ni, &res)) {
                /* reading an undeclared name is an error HERE at compile time -
                 * no undefined-variable surprises at runtime */
                return comp_error_tok(vm, t, "unknown identifier '%s'",
                                      vm->names[ni].name);
            }
            switch (res.kind) {
                case RESOLVE_LOCAL:
                    pi->is_var = true;
                    pi->is_local = true;
                    pi->is_const = res.is_const;
                    pi->slot = res.slot;
                    return emit_op_u32(vm, OP_LOAD_LOCAL, res.slot);
                case RESOLVE_GLOBAL:
                    pi->is_var = true;
                    pi->is_local = false;
                    pi->is_const = res.is_const;
                    pi->slot = res.idx;
                    return emit_op_u32(vm, OP_LOAD_GLOBAL, res.idx);
                case RESOLVE_BUILTIN:
                    return push_native_value(vm, res.idx);
                default:
                    return VANTA_ERR_INTERNAL;
            }
        }

        default:
            return comp_error_tok(vm, t, "expected expression, got %s",
                                  token_kind_name(t->kind));
    }
}

/* Natives ride OP_MAKE_FN with the high bit set; function_count is bounded
 * far below 2^31 so the tag is unambiguous. The interpreter materializes a
 * V_NATIVE value from the low bits. */
static VantaResult push_native_value(VantaVM *vm, uint32_t native_id) {
    return emit_op_u32(vm, OP_MAKE_FN, native_id | 0x80000000u);
}

/* unary and postfix */

static VantaResult compile_unary(VantaVM *vm, PrimaryInfo *pi) {
    const VantaToken *t = tok_cur(vm);

    switch (t->kind) {
        case TK_MINUS: {
            note_source_line(vm, t->line);
            tok_advance(vm);
            /* -x: fold constant literals at compile time (dense bytecode for
             * numeric tables), otherwise emit NEG */
            const VantaToken *nt = tok_cur(vm);
            if (nt->kind == TK_INT) {
                tok_advance(vm);
                VantaResult r = emit_op(vm, OP_PUSH_INT);
                if (r) return r;
                int64_t v = nt->v.i;
                /* careful: -(INT64_MIN) does not fit; emit runtime NEG for it */
                if (v == (-9223372036854775807LL - 1LL)) {
                    r = emit_i64(vm, v);
                    if (r) return r;
                    return emit_op(vm, OP_NEG);
                }
                return emit_i64(vm, -v);
            }
            if (nt->kind == TK_FLOAT) {
                tok_advance(vm);
                VantaResult r = emit_op(vm, OP_PUSH_FLOAT);
                if (r) return r;
                return emit_f64(vm, -nt->v.f);
            }
            {
                VantaResult r = compile_unary(vm, pi);
                if (r) return r;
                pi->is_var = false;
                return emit_op(vm, OP_NEG);
            }
        }

        case TK_PLUS:
            /* unary plus is a numeric identity; accept and ignore */
            note_source_line(vm, t->line);
            tok_advance(vm);
            return compile_unary(vm, pi);

        case TK_BANG:
            note_source_line(vm, t->line);
            tok_advance(vm);
            {
                VantaResult r = compile_unary(vm, pi);
                if (r) return r;
                pi->is_var = false;
                return emit_op(vm, OP_NOT);
            }

        case TK_TILDE:
            note_source_line(vm, t->line);
            tok_advance(vm);
            {
                VantaResult r = compile_unary(vm, pi);
                if (r) return r;
                pi->is_var = false;
                return emit_op(vm, OP_BNOT);
            }

        case TK_PLUSPLUS:
        case TK_MINUSMINUS: {
            /* prefix form: only a declared variable may follow */
            note_source_line(vm, t->line);
            bool inc = (t->kind == TK_PLUSPLUS);
            tok_advance(vm);
            if (!tok_is(vm, TK_IDENT)) {
                return comp_error_tok(vm, tok_cur(vm),
                                      "%s requires a variable target",
                                      inc ? "'++'" : "'--'");
            }
            const VantaToken *vt = tok_cur(vm);
            tok_advance(vm);
            int32_t ni = name_index(vm, vm->source + vt->src_off, vt->v.len);
            if (ni < 0) return comp_error_tok(vm, vt, "identifier table exhausted");
            ResolveOut res;
            if (!resolve_ident(vm, (uint32_t)ni, &res)) {
                return comp_error_tok(vm, vt, "unknown identifier '%s'", vm->names[ni].name);
            }
            if (res.kind != RESOLVE_LOCAL && res.kind != RESOLVE_GLOBAL) {
                return comp_error_tok(vm, vt, "%s requires a variable target",
                                      inc ? "'++'" : "'--'");
            }
            if (res.is_const) {
                return comp_error_tok(vm, vt, "cannot modify const '%s'",
                                      vm->names[ni].name);
            }
            if (res.kind == RESOLVE_LOCAL) {
                VantaResult r = emit_op_u32(vm, inc ? OP_INC_LOCAL : OP_DEC_LOCAL, res.slot);
                if (r) return r;
                r = emit_op_u32(vm, OP_LOAD_LOCAL, res.slot);
                if (r) return r;
            } else {
                /* global: LOAD, +-1, ADD, STORE leaves the NEW value on stack */
                VantaResult r = emit_op_u32(vm, OP_LOAD_GLOBAL, res.idx);
                if (r) return r;
                r = emit_op(vm, OP_PUSH_INT);
                if (r) return r;
                r = emit_i64(vm, 1);
                if (r) return r;
                r = emit_op(vm, inc ? OP_ADD : OP_SUB);
                if (r) return r;
                /* DUP the result before STORE consumes it: prefix ++ yields
                 * the new value as an expression result */
                r = emit_op(vm, OP_DUP);
                if (r) return r;
                r = emit_op_u32(vm, OP_STORE_GLOBAL, res.idx);
                if (r) return r;
            }
            pi->is_var = true;
            pi->is_local = (res.kind == RESOLVE_LOCAL);
            pi->is_const = false;
            pi->slot = (res.kind == RESOLVE_LOCAL) ? res.slot : res.idx;
            pi->name_idx = (uint32_t)ni;
            /* postfix chain may continue: (++x).y etc. - rare but legal */
            return compile_postfix_loop(vm, pi);
        }

        default: {
            VantaResult r = compile_primary(vm, pi);
            if (r) return r;
            return compile_postfix_loop(vm, pi);
        }
    }
}

/* call argument list */

static VantaResult compile_call_args(VantaVM *vm, uint32_t *argc_out) {
    /* current token is '(' */
    tok_advance(vm);
    uint32_t argc = 0;
    skip_nl(vm);
    while (!tok_is(vm, TK_RPAREN)) {
        if (argc >= VANTA_MAX_ARGS) {
            return comp_error_tok(vm, tok_cur(vm),
                                  "call exceeds %u arguments", VANTA_MAX_ARGS);
        }
        VantaResult r = compile_expr_prec(vm, PREC_TERNARY);
        if (r) return r;
        argc++;
        skip_nl(vm);
        if (tok_is(vm, TK_COMMA)) { tok_advance(vm); skip_nl(vm); continue; }
        break;
    }
    if (!tok_is(vm, TK_RPAREN)) {
        return comp_error_tok(vm, tok_cur(vm), "expected ')' to close argument list");
    }
    tok_advance(vm);
    *argc_out = argc;
    return VANTA_OK;
}

/* postfix chain: calls, indexing, member access, postfix ++/-- */

static VantaResult compile_postfix_loop(VantaVM *vm, PrimaryInfo *pi) {
    for (;;) {
        const VantaToken *t = tok_cur(vm);

        if (t->kind == TK_LPAREN) {
            note_source_line(vm, t->line);
            uint32_t argc;
            VantaResult r = compile_call_args(vm, &argc);
            if (r) return r;
            r = emit_op_u32(vm, OP_CALL, argc);
            if (r) return r;
            pi->is_var = false;
            continue;
        }

        if (t->kind == TK_LBRACKET) {
            note_source_line(vm, t->line);
            tok_advance(vm);
            skip_nl(vm);
            VantaResult r = compile_expr_prec(vm, PREC_TERNARY);
            if (r) return r;
            skip_nl(vm);
            r = expect_tok(vm, TK_RBRACKET);
            if (r) return r;
            r = emit_op(vm, OP_INDEX);
            if (r) return r;
            pi->is_var = false;
            continue;
        }

        if (t->kind == TK_DOT) {
            note_source_line(vm, t->line);
            tok_advance(vm);
            if (!tok_is(vm, TK_IDENT)) {
                return comp_error_tok(vm, tok_cur(vm),
                                      "expected member name after '.', got %s",
                                      token_kind_name(tok_cur(vm)->kind));
            }
            const VantaToken *mt = tok_cur(vm);
            tok_advance(vm);

            uint32_t mid = method_lookup(vm->source + mt->src_off, mt->v.len);
            if (mid != VANTA_NO_INDEX) {
                if (tok_is(vm, TK_LPAREN)) {
                    uint32_t argc;
                    VantaResult r = compile_call_args(vm, &argc);
                    if (r) return r;
                    r = emit_op(vm, OP_CALL_METHOD);
                    if (r) return r;
                    r = emit_u32(vm, (mid << 16) | argc);
                    if (r) return r;
                } else {
                    /* paren-less method == zero-arg method (m.len) */
                    VantaResult r = emit_op(vm, OP_CALL_METHOD);
                    if (r) return r;
                    r = emit_u32(vm, (mid << 16));
                    if (r) return r;
                }
                pi->is_var = false;
                continue;
            }

            /* not a method: map field read. Key becomes a string constant and
             * the access is an INDEX - maps are the only dotted containers. */
            uint32_t key_const;
            VantaResult r = add_const_string(vm, vm->source + mt->src_off, mt->v.len,
                                             &key_const);
            if (r) return r;
            r = emit_op_u32(vm, OP_PUSH_CONST, key_const);
            if (r) return r;
            r = emit_op(vm, OP_INDEX);
            if (r) return r;
            pi->is_var = false;
            continue;
        }

        if (t->kind == TK_PLUSPLUS || t->kind == TK_MINUSMINUS) {
            bool inc = (t->kind == TK_PLUSPLUS);
            if (!pi->is_var || pi->is_const) {
                if (pi->is_var && pi->is_const) {
                    return comp_error_tok(vm, t, "cannot modify a const");
                }
                return comp_error_tok(vm, t,
                                      "postfix %s requires a variable target",
                                      inc ? "'++'" : "'--'");
            }
            note_source_line(vm, t->line);
            tok_advance(vm);
            /* old value already on the stack from the LOAD emitted by
             * compile_primary; mutate the slot underneath it */
            VantaResult r;
            if (pi->is_local) {
                r = emit_op_u32(vm, inc ? OP_INC_LOCAL : OP_DEC_LOCAL, pi->slot);
            } else {
                /* global: stack [old]; recompute new = old +- 1 without
                 * clobbering: LOAD again, +-1, STORE (STORE pops the new). */
                r = emit_op_u32(vm, OP_LOAD_GLOBAL, pi->slot);
                if (r) return r;
                r = emit_op(vm, OP_PUSH_INT);
                if (r) return r;
                r = emit_i64(vm, 1);
                if (r) return r;
                r = emit_op(vm, inc ? OP_ADD : OP_SUB);
                if (r) return r;
                r = emit_op_u32(vm, OP_STORE_GLOBAL, pi->slot);
            }
            if (r) return r;
            pi->is_var = false;
            continue;
        }

        break;
    }
    return VANTA_OK;
}

/* function expressions */

static VantaResult compile_fn_expression(VantaVM *vm, const VantaToken *fn_tok,
                                         PrimaryInfo *pi) {
    /* cursor sits on 'fn'; may be followed by a name (named function
     * expression registers a global so recursion and later calls resolve) */
    tok_advance(vm);   /* consume 'fn' */

    int32_t name_ni = -1;
    if (tok_is(vm, TK_IDENT)) {
        const VantaToken *nt = tok_cur(vm);
        name_ni = name_index(vm, vm->source + nt->src_off, nt->v.len);
        if (name_ni < 0) return comp_error_tok(vm, nt, "identifier table exhausted");
        tok_advance(vm);
    }

    VantaResult r = expect_tok(vm, TK_LPAREN);
    if (r) return r;

    /* parameters */
    uint32_t param_name_idx[VANTA_MAX_PARAMS];
    uint32_t nparams = 0;
    skip_nl(vm);
    while (!tok_is(vm, TK_RPAREN)) {
        if (!tok_is(vm, TK_IDENT)) {
            return comp_error_tok(vm, tok_cur(vm),
                                  "parameter must be an identifier, got %s",
                                  token_kind_name(tok_cur(vm)->kind));
        }
        if (nparams >= VANTA_MAX_PARAMS) {
            return comp_error_tok(vm, tok_cur(vm),
                                  "function exceeds %u parameters", VANTA_MAX_PARAMS);
        }
        const VantaToken *pt = tok_cur(vm);
        int32_t pni = name_index(vm, vm->source + pt->src_off, pt->v.len);
        if (pni < 0) return comp_error_tok(vm, pt, "identifier table exhausted");
        for (uint32_t i = 0; i < nparams; i++) {
            if (param_name_idx[i] == (uint32_t)pni) {
                return comp_error_tok(vm, pt, "duplicate parameter '%s'",
                                      vm->names[pni].name);
            }
        }
        param_name_idx[nparams++] = (uint32_t)pni;
        tok_advance(vm);
        skip_nl(vm);
        if (tok_is(vm, TK_COMMA)) { tok_advance(vm); skip_nl(vm); continue; }
        break;
    }
    r = expect_tok(vm, TK_RPAREN);
    if (r) return r;

    /* register the function record now so the body can reference itself */
    if (vm->function_count >= VANTA_MAX_FUNCTIONS) {
        return comp_error_tok(vm, fn_tok, "function limit (%u) exceeded",
                              VANTA_MAX_FUNCTIONS);
    }
    uint32_t fn_idx = vm->function_count++;
    VantaFunction *f = &vm->functions[fn_idx];
    f->name_idx = (name_ni >= 0) ? (uint32_t)name_ni : VANTA_NO_INDEX;
    f->code_off = VANTA_NO_INDEX;
    f->nparams = nparams;
    f->nlocals = nparams;
    for (uint32_t i = 0; i < nparams; i++) {
        f->param_name[i] = (uint16_t)param_name_idx[i];
    }
    f->pad = 0;

    /* Named expression functions bind a global so `fn fact(n) {...}` can
     * recurse and later statements can call it. Top-level hoisting happens
     * in the pre-scan (P15). */
    int32_t gslot = -1;
    if (name_ni >= 0) {
        gslot = global_declare(vm, (uint32_t)name_ni, GLOBAL_FLAG_DEFINED);
        if (gslot < 0) return comp_error_tok(vm, fn_tok, "global table exhausted");
    }

    /* MAKE_FN lands on the stack: an anonymous function expression leaves it
     * there for the surrounding expression to consume; a named one stores it
     * into its global. The JMP below skips the body bytes - they are only
     * ever entered through a CALL. */
    r = emit_op_u32(vm, OP_MAKE_FN, fn_idx);
    if (r) return r;
    uint32_t over_body;
    if (gslot >= 0) {
        r = emit_op_u32(vm, OP_STORE_GLOBAL, (uint32_t)gslot);
        if (r) return r;
    }
    r = emit_jump(vm, OP_JMP, &over_body);
    if (r) return r;

    /* body */
    CompilerFnState outer;
    fn_state_save(vm, &outer);

    vm->cur_fn = fn_idx;
    vm->local_count = 0;
    vm->localdesc_count = 0;
    vm->scope_depth = 0;
    vm->loop_depth = 0;

    /* parameter slots come first, in declaration order */
    for (uint32_t i = 0; i < nparams; i++) {
        uint32_t slot;
        r = local_declare_param(vm, param_name_idx[i], &slot);
        if (r) { fn_state_restore(vm, &outer); return r; }
        if (slot != i) { fn_state_restore(vm, &outer); return VANTA_ERR_INTERNAL; }
    }

    f->code_off = bc_pos(vm);

    r = expect_tok(vm, TK_LBRACE);
    if (r) { fn_state_restore(vm, &outer); return r; }

    enter_scope(vm);
    skip_nl(vm);
    while (!tok_is(vm, TK_RBRACE) && !tok_is(vm, TK_EOF)) {
        r = compile_statement(vm, false);
        if (r) { fn_state_restore(vm, &outer); return r; }
        skip_nl(vm);
    }
    if (!tok_is(vm, TK_RBRACE)) {
        r = comp_error_tok(vm, tok_cur(vm), "expected '}' to close function body");
        fn_state_restore(vm, &outer);
        return r;
    }
    tok_advance(vm);
    exit_scope(vm, 0);

    f->nlocals = vm->local_count;

    /* implicit nil return */
    r = emit_op(vm, OP_RETURN_NIL);
    fn_state_restore(vm, &outer);
    if (r) return r;

    patch_jump(vm, over_body, bc_pos(vm));

    pi->is_var = false;
    return VANTA_OK;
}

/* precedence climbing */

static VantaResult compile_expr_prec(VantaVM *vm, uint32_t min_prec) {
    PrimaryInfo pi;
    VantaResult r = compile_unary(vm, &pi);
    if (r) return r;

    for (;;) {
        const VantaToken *t = tok_cur(vm);

        if (t->kind == TK_QUESTION && min_prec <= PREC_TERNARY) {
            note_source_line(vm, t->line);
            tok_advance(vm);
            uint32_t else_patch, end_patch;
            r = emit_jump(vm, OP_JMP_FALSE_POP, &else_patch);
            if (r) return r;
            r = compile_expr_prec(vm, PREC_TERNARY);
            if (r) return r;
            r = emit_jump(vm, OP_JMP, &end_patch);
            if (r) return r;
            patch_jump(vm, else_patch, bc_pos(vm));
            skip_nl(vm);
            r = expect_tok(vm, TK_COLON);
            if (r) return r;
            r = compile_expr_prec(vm, PREC_TERNARY);   /* right-assoc */
            if (r) return r;
            patch_jump(vm, end_patch, bc_pos(vm));
            continue;
        }

        BinOpInfo bi;
        if (!binop_info(t->kind, &bi)) break;
        if (bi.prec < min_prec) break;

        note_source_line(vm, t->line);
        tok_advance(vm);

        if (t->kind == TK_AND || t->kind == TK_OR) {
            /* short-circuit: result is a BOOL. If the left operand decides,
             * the fallback constant is pushed; otherwise the right operand's
             * truthiness decides and its value is discarded to a bool.
            */
            
            uint32_t short_patch;
            r = emit_jump(vm, t->kind == TK_AND ? OP_JMP_FALSE_POP : OP_JMP_TRUE_POP,
                          &short_patch);
            if (r) return r;
            r = compile_expr_prec(vm, bi.prec + 1);
            if (r) return r;
            r = emit_op(vm, OP_NOT);
            if (r) return r;
            r = emit_op(vm, OP_NOT);      /* double NOT normalizes truthy values
                                           * of ANY type to a real bool without
                                           * type restrictions */
            if (r) return r;
            uint32_t end_patch;
            r = emit_jump(vm, OP_JMP, &end_patch);
            if (r) return r;
            patch_jump(vm, short_patch, bc_pos(vm));
            r = emit_op(vm, t->kind == TK_AND ? OP_PUSH_FALSE : OP_PUSH_TRUE);
            if (r) return r;
            patch_jump(vm, end_patch, bc_pos(vm));
            continue;
        }

        r = compile_expr_prec(vm, bi.prec + 1);   /* all listed binops are left-assoc */
        if (r) return r;
        r = emit_op(vm, bi.op);
        if (r) return r;
    }

    return VANTA_OK;
}

/* full expression at statement level */
static VantaResult compile_expr(VantaVM *vm) {
    return compile_expr_prec(vm, PREC_TERNARY);
}

/* P15 - statement compiler and program driver
 *
 * Statement dispatch, let/const declarations, control flow with a real
 * loop/patch machinery, and assignment to arbitrary lvalue paths
 * (x, x[i], x.k, x[i].k, x[i][j]...). Assignment first SCANS the target
 * shape over tokens only; it only commits to emission when the scan ends on
 * an assignment operator. Failed scans rewind the cursor and fall back to a
 * normal expression statement - so `foo(bar)` and `arr[0] + 1` never pay
 * for the assignment machinery. */

/* assignment scanning */

#define ASSIGN_DEPTH_MAX 10u

typedef enum { STEP_INDEX, STEP_KEY } AssignStepKind;

typedef struct {
    AssignStepKind kind;
    uint32_t tok_start;      /* first token of the subscript expr */
    uint32_t tok_end;        /* one past the closing ']'          */
    uint32_t key_off;        /* STEP_KEY: source offset of the name */
    uint32_t key_len;
} AssignStep;

/* Scan forward past an lvalue prefix that BEGINS with an identifier. On
 * return the cursor sits on the token AFTER the target; *is_assign tells
 * whether that token is an assignment operator. Pure token walk - nothing
 * is emitted, so a rejected scan costs nothing but a cursor restore. */
static VantaResult scan_assign_target(VantaVM *vm, AssignStep *steps, uint32_t *step_count,
                                      bool *is_assign) {
    /* cursor is on the identifier; consume it */
    tok_advance(vm);
    skip_nl(vm);
    *step_count = 0;
    *is_assign = false;

    for (;;) {
        if (tok_is(vm, TK_LBRACKET)) {
            if (*step_count >= ASSIGN_DEPTH_MAX) {
                return comp_error_tok(vm, tok_cur(vm), "assignment path too deep");
            }
            AssignStep *s = &steps[(*step_count)++];
            s->kind = STEP_INDEX;
            s->tok_start = vm->cur_token + 1;
            tok_advance(vm);
            /* skip to matching ']': depth count across (), [], {} */
            int depth = 1;
            while (depth > 0) {
                uint32_t k = tok_cur(vm)->kind;
                if (k == TK_EOF) {
                    return comp_error_tok(vm, tok_cur(vm), "unterminated subscript");
                }
                if (k == TK_LBRACKET || k == TK_LPAREN || k == TK_LBRACE) depth++;
                else if (k == TK_RBRACKET || k == TK_RPAREN || k == TK_RBRACE) depth--;
                if (depth == 0) break;
                tok_advance(vm);
            }
            /* cursor on ']' */
            s->tok_end = vm->cur_token;
            tok_advance(vm);
            skip_nl(vm);
            continue;
        }
        if (tok_is(vm, TK_DOT)) {
            tok_advance(vm);
            if (!tok_is(vm, TK_IDENT)) {
                /* not an lvalue dot-access: bail out as non-assignment */
                *is_assign = false;
                return VANTA_OK;
            }
            /* a method call (.name( ) cannot be an assignment target */
            if (vm->cur_token + 1 < vm->token_count &&
                vm->tokens[vm->cur_token + 1].kind == TK_LPAREN) {
                *is_assign = false;
                return VANTA_OK;
            }
            if (*step_count >= ASSIGN_DEPTH_MAX) {
                return comp_error_tok(vm, tok_cur(vm), "assignment path too deep");
            }
            AssignStep *s = &steps[(*step_count)++];
            s->kind = STEP_KEY;
            s->tok_start = vm->cur_token;
            s->tok_end = vm->cur_token + 1;
            const VantaToken *nt = tok_cur(vm);
            s->key_off = nt->src_off;
            s->key_len = nt->v.len;
            tok_advance(vm);
            skip_nl(vm);
            continue;
        }
        break;
    }

    switch (tok_cur(vm)->kind) {
        case TK_ASSIGN:
        case TK_PLUS_ASSIGN: case TK_MINUS_ASSIGN: case TK_STAR_ASSIGN:
        case TK_SLASH_ASSIGN: case TK_PERCENT_ASSIGN:
        case TK_BAND_ASSIGN: case TK_BOR_ASSIGN: case TK_BXOR_ASSIGN:
        case TK_SHL_ASSIGN: case TK_SHR_ASSIGN:
            *is_assign = true;
            break;
        default:
            *is_assign = false;
            break;
    }
    return VANTA_OK;
}

static uint32_t assign_op_opcode(uint32_t kind) {
    switch (kind) {
        case TK_PLUS_ASSIGN:    return OP_ADD;
        case TK_MINUS_ASSIGN:   return OP_SUB;
        case TK_STAR_ASSIGN:    return OP_MUL;
        case TK_SLASH_ASSIGN:   return OP_DIV;
        case TK_PERCENT_ASSIGN: return OP_MOD;
        case TK_BAND_ASSIGN:    return OP_BAND;
        case TK_BOR_ASSIGN:     return OP_BOR;
        case TK_BXOR_ASSIGN:    return OP_BXOR;
        case TK_SHL_ASSIGN:     return OP_SHL;
        case TK_SHR_ASSIGN:     return OP_SHR;
        default:                return VANTA_NO_INDEX;   /* plain '=' */
    }
}

/* Emit the full assignment. `ident` is the first token (identifier name).
 * Steps were scanned by scan_assign_target; the cursor now sits PAST the
 * assignment operator.
*/

static VantaResult compile_assignment(VantaVM *vm, const VantaToken *ident,
                                      const AssignStep *steps, uint32_t step_count,
                                      uint32_t op_kind) {
    const char *name_src = vm->source + ident->src_off;
    uint32_t name_len = ident->v.len;
    int32_t ni = name_index(vm, name_src, name_len);
    if (ni < 0) return comp_error_tok(vm, ident, "identifier table exhausted");

    ResolveOut res;
    if (!resolve_ident(vm, (uint32_t)ni, &res)) {
        return comp_error_tok(vm, ident,
                              "cannot assign to '%s': identifier not declared (use let/const)",
                              vm->names[ni].name);
    }
    if (res.kind == RESOLVE_BUILTIN) {
        return comp_error_tok(vm, ident, "cannot assign to builtin '%s'", vm->names[ni].name);
    }
    if (res.is_const) {
        return comp_error_tok(vm, ident, "cannot assign to const '%s'", vm->names[ni].name);
    }

    uint32_t binop = assign_op_opcode(op_kind);
    VantaResult r;

    if (step_count == 0) {
        /* simple variable target */
        uint32_t store_op, load_op, slot;
        if (res.kind == RESOLVE_LOCAL) {
            store_op = OP_STORE_LOCAL;
            load_op = OP_LOAD_LOCAL;
            slot = res.slot;
        } else {
            store_op = OP_STORE_GLOBAL;
            load_op = OP_LOAD_GLOBAL;
            slot = res.idx;
        }
        if (binop != VANTA_NO_INDEX) {
            r = emit_op_u32(vm, load_op, slot);
            if (r) return r;
            r = compile_expr(vm);
            if (r) return r;
            r = emit_op(vm, binop);
            if (r) return r;
        } else {
            r = compile_expr(vm);
            if (r) return r;
        }
        return emit_op_u32(vm, store_op, slot);
    }

    /* path target: load the root, then every intermediate access via INDEX */
    if (res.kind == RESOLVE_LOCAL) {
        r = emit_op_u32(vm, OP_LOAD_LOCAL, res.slot);
    } else {
        r = emit_op_u32(vm, OP_LOAD_GLOBAL, res.idx);
    }
    if (r) return r;

    for (uint32_t i = 0; i < step_count - 1; i++) {
        const AssignStep *s = &steps[i];
        if (s->kind == STEP_INDEX) {
            uint32_t save = vm->cur_token;
            vm->cur_token = s->tok_start;
            r = compile_expr(vm);
            if (r) return r;
            if (vm->cur_token != s->tok_end) {
                return comp_error_at(vm, tok_cur(vm)->line, tok_cur(vm)->col,
                                     "subscript parse mismatch in assignment target");
            }
            vm->cur_token = save;
            r = emit_op(vm, OP_INDEX);
            if (r) return r;
        } else {
            uint32_t cidx;
            r = add_const_string(vm, vm->source + s->key_off, s->key_len, &cidx);
            if (r) return r;
            r = emit_op_u32(vm, OP_PUSH_CONST, cidx);
            if (r) return r;
            r = emit_op(vm, OP_INDEX);
            if (r) return r;
        }
    }

    /* last step: leave [obj, key] prepared, then DUP2 to preserve them
     * through the read-modify-write of compound assignment */
    const AssignStep *last = &steps[step_count - 1];
    if (last->kind == STEP_INDEX) {
        uint32_t save = vm->cur_token;
        vm->cur_token = last->tok_start;
        r = compile_expr(vm);
        if (r) return r;
        if (vm->cur_token != last->tok_end) {
            return comp_error_at(vm, tok_cur(vm)->line, tok_cur(vm)->col,
                                 "subscript parse mismatch in assignment target");
        }
        vm->cur_token = save;
    } else {
        uint32_t cidx;
        r = add_const_string(vm, vm->source + last->key_off, last->key_len, &cidx);
        if (r) return r;
        r = emit_op_u32(vm, OP_PUSH_CONST, cidx);
        if (r) return r;
    }
    r = emit_op(vm, OP_DUP2);    /* [obj, key] -> [obj, key, obj, key] */
    if (r) return r;

    if (binop == VANTA_NO_INDEX) {
        r = compile_expr(vm);
        if (r) return r;
        return emit_op(vm, OP_INDEX_SET);        /* consumes obj,key,val */
    }

    r = emit_op(vm, OP_INDEX);                    /* [obj, key, old] */
    if (r) return r;
    r = compile_expr(vm);
    if (r) return r;
    r = emit_op(vm, binop);                       /* [obj, key, new] */
    if (r) return r;
    return emit_op(vm, OP_INDEX_SET);
}

/* declarations */

static VantaResult compile_let(VantaVM *vm, bool is_const) {
    tok_advance(vm);   /* 'let' / 'const' */
    if (!tok_is(vm, TK_IDENT)) {
        return comp_error_tok(vm, tok_cur(vm), "expected identifier after %s",
                              is_const ? "'const'" : "'let'");
    }

    /* multi-declaration: let a = 1, b = 2 */
    VantaResult r;
    for (;;) {
        if (!tok_is(vm, TK_IDENT)) break;
        const VantaToken *id_tok = tok_cur(vm);
        tok_advance(vm);

        int32_t ni = name_index(vm, vm->source + id_tok->src_off, id_tok->v.len);
        if (ni < 0) return comp_error_tok(vm, id_tok, "identifier table exhausted");

        /* initializer is optional ONLY for mutable locals (nil seed) */
        bool has_init = tok_is(vm, TK_ASSIGN);
        if (has_init) tok_advance(vm);

        if (!has_init && is_const) {
            return comp_error_tok(vm, id_tok, "const '%s' requires an initializer",
                                  vm->names[ni].name);
        }

        /* declare FIRST so the initializer may reference the binding
         * recursively (arrays that push themselves during construction) */
        uint32_t slot;
        r = local_declare(vm, (uint32_t)ni, is_const,
                          id_tok->line, id_tok->col, &slot);
        if (r) return r;

        if (has_init) {
            r = compile_expr(vm);
        } else {
            r = emit_op(vm, OP_PUSH_NIL);
        }
        if (r) return r;
        r = emit_op_u32(vm, OP_STORE_LOCAL, slot);
        if (r) return r;

        if (tok_is(vm, TK_COMMA)) {
            tok_advance(vm);
            skip_nl(vm);
            continue;
        }
        break;
    }
    return VANTA_OK;
}

/* blocks and single statements */

static VantaResult compile_block(VantaVM *vm) {
    /* cursor on '{' */
    tok_advance(vm);
    enter_scope(vm);
    uint32_t depth = vm->scope_depth;
    skip_nl(vm);
    VantaResult r = VANTA_OK;
    while (!tok_is(vm, TK_RBRACE) && !tok_is(vm, TK_EOF)) {
        r = compile_statement(vm, false);
        if (r) return r;
        skip_nl(vm);
    }
    if (!tok_is(vm, TK_RBRACE)) {
        return comp_error_tok(vm, tok_cur(vm), "expected '}' to close block");
    }
    tok_advance(vm);
    exit_scope(vm, depth - 1);
    return VANTA_OK;
}

static VantaResult compile_body(VantaVM *vm) {
    /* loop/if body: block or a single statement */
    if (tok_is(vm, TK_LBRACE)) return compile_block(vm);
    return compile_statement(vm, false);
}

/* control flow */

static VantaResult compile_if(VantaVM *vm) {
    tok_advance(vm);   /* 'if' */
    VantaResult r = compile_expr(vm);
    if (r) return r;

    uint32_t then_end;
    r = emit_jump(vm, OP_JMP_FALSE_POP, &then_end);
    if (r) return r;
    skip_nl(vm);
    r = compile_body(vm);
    if (r) return r;

    if (tok_is(vm, TK_K_ELSE)) {
        tok_advance(vm);
        skip_nl(vm);
        uint32_t else_end;
        r = emit_jump(vm, OP_JMP, &else_end);
        if (r) return r;
        patch_jump(vm, then_end, bc_pos(vm));
        if (tok_is(vm, TK_K_IF)) {
            r = compile_if(vm);        /* else-if chain */
        } else {
            r = compile_body(vm);
        }
        if (r) return r;
        patch_jump(vm, else_end, bc_pos(vm));
        return VANTA_OK;
    }

    patch_jump(vm, then_end, bc_pos(vm));
    return VANTA_OK;
}

static VantaResult compile_while(VantaVM *vm) {
    const VantaToken *w = tok_cur(vm);
    tok_advance(vm);   /* 'while' */

    VantaResult r = push_loop(vm, vm->cur_fn);
    if (r) return r;
    VantaLoop *loop = cur_loop(vm);
    loop->continue_pc = bc_pos(vm);   /* continue re-evaluates the condition */

    r = compile_expr(vm);
    if (r) return r;
    uint32_t end_patch;
    r = emit_jump(vm, OP_JMP_FALSE_POP, &end_patch);
    if (r) return r;

    skip_nl(vm);
    enter_scope(vm);
    uint32_t depth = vm->scope_depth;
    r = compile_body(vm);
    exit_scope(vm, depth - 1);
    if (r) return r;

    r = emit_op_u32(vm, OP_JMP, loop->continue_pc);
    if (r) return r;

    uint32_t end_pc = bc_pos(vm);
    patch_jump(vm, end_patch, end_pc);
    patch_list_resolve(vm, loop->break_patches, end_pc);
    patch_list_resolve(vm, loop->continue_patches, loop->continue_pc);
    pop_loop(vm);
    (void)w;
    return VANTA_OK;
}

static VantaResult compile_for_classic(VantaVM *vm, bool has_init) {
    /* cursor sits after 'for'; classic three-part form uses ';' separators.
     * `for let i = 0; ...` scopes the counter to the loop. */
    VantaResult r;

    enter_scope(vm);
    uint32_t scope_entry = vm->scope_depth;

    if (has_init) {
        if (tok_is(vm, TK_K_LET) || tok_is(vm, TK_K_CONST)) {
            r = compile_let(vm, tok_is(vm, TK_K_CONST));
            if (r) { exit_scope(vm, scope_entry); return r; }
            /* compile_let leaves the cursor on its terminator: the classic
             * for needs the ';' itself */
            if (!tok_is(vm, TK_SEMI)) {
                r = comp_error_tok(vm, tok_cur(vm), "expected ';' after for-initializer");
                exit_scope(vm, scope_entry);
                return r;
            }
            tok_advance(vm);
        } else {
            /* assignment/expression initializer: compile_statement consumes
             * the ';' as its statement terminator - do not expect another */
            r = compile_statement(vm, false);
            if (r) { exit_scope(vm, scope_entry); return r; }
        }
    } else {
        if (!tok_is(vm, TK_SEMI)) {
            r = comp_error_tok(vm, tok_cur(vm), "expected ';' after for-initializer");
            exit_scope(vm, scope_entry);
            return r;
        }
        tok_advance(vm);
    }

    r = push_loop(vm, vm->cur_fn);
    if (r) { exit_scope(vm, scope_entry); return r; }
    VantaLoop *loop = cur_loop(vm);

    /* condition */
    uint32_t cond_pc = bc_pos(vm);
    uint32_t end_patch;
    if (tok_is(vm, TK_SEMI)) {
        /* empty condition = forever */
        tok_advance(vm);
        r = emit_op(vm, OP_PUSH_TRUE);
        if (r) { exit_scope(vm, scope_entry); pop_loop(vm); return r; }
        r = emit_jump(vm, OP_JMP_FALSE_POP, &end_patch);
        if (r) { exit_scope(vm, scope_entry); pop_loop(vm); return r; }
        /* end_patch never taken but must resolve: patch to end below */
    } else {
        r = compile_expr(vm);
        if (r) { exit_scope(vm, scope_entry); pop_loop(vm); return r; }
        if (!tok_is(vm, TK_SEMI)) {
            r = comp_error_tok(vm, tok_cur(vm), "expected ';' after for-condition");
            exit_scope(vm, scope_entry); pop_loop(vm);
            return r;
        }
        tok_advance(vm);
        r = emit_jump(vm, OP_JMP_FALSE_POP, &end_patch);
        if (r) { exit_scope(vm, scope_entry); pop_loop(vm); return r; }
    }

    /* update expression: parsed LAST but emitted after the body. Scan it
     * out of the way first (token range), emit body, then emit update and
     * wire continue there. */
    uint32_t upd_tok_start, upd_tok_end;
    if (tok_is(vm, TK_LBRACE)) {
        upd_tok_start = vm->cur_token;
        upd_tok_end = vm->cur_token;      /* no update: body opens immediately */
    } else {
        upd_tok_start = vm->cur_token;
        /* skip to the body '{' at bracket depth 0. All bracket kinds count:
         * an update expression containing a map literal must not terminate
         * the scan at the literal's own braces. */
        int depth = 0;
        while (true) {
            uint32_t k = tok_cur(vm)->kind;
            if (k == TK_EOF) {
                r = comp_error_tok(vm, tok_cur(vm), "expected '{' to open for-body");
                exit_scope(vm, scope_entry); pop_loop(vm);
                return r;
            }
            if (k == TK_LBRACE && depth == 0) break;
            if (k == TK_LPAREN || k == TK_LBRACKET || k == TK_LBRACE) depth++;
            else if (k == TK_RPAREN || k == TK_RBRACKET || k == TK_RBRACE) {
                if (depth == 0) {
                    r = comp_error_tok(vm, tok_cur(vm), "expected '{' to open for-body");
                    exit_scope(vm, scope_entry); pop_loop(vm);
                    return r;
                }
                depth--;
            }
            tok_advance(vm);
        }
        upd_tok_end = vm->cur_token;
        /* cursor on '{' */
    }

    skip_nl(vm);
    r = compile_body(vm);
    if (r) { exit_scope(vm, scope_entry); pop_loop(vm); return r; }

    uint32_t upd_pc = bc_pos(vm);
    loop->continue_pc = upd_pc;

    if (upd_tok_end > upd_tok_start) {
        uint32_t save = vm->cur_token;
        vm->cur_token = upd_tok_start;
        r = compile_expr(vm);
        if (!r) r = emit_op(vm, OP_POP);
        vm->cur_token = save;
        if (r) { exit_scope(vm, scope_entry); pop_loop(vm); return r; }
    } else if (tok_is(vm, TK_EOF)) {
        /* unreachable: body parser consumed the braces */
    }

    r = emit_op_u32(vm, OP_JMP, cond_pc);
    if (r) { exit_scope(vm, scope_entry); pop_loop(vm); return r; }

    uint32_t end_pc = bc_pos(vm);
    patch_jump(vm, end_patch, end_pc);
    patch_list_resolve(vm, loop->break_patches, end_pc);
    patch_list_resolve(vm, loop->continue_patches, upd_pc);
    pop_loop(vm);
    exit_scope(vm, scope_entry);
    return VANTA_OK;
}

static VantaResult compile_for_in(VantaVM *vm) {
    /* cursor after 'for'; grammar: for [let] IDENT in EXPR { BODY } */
    VantaResult r;

    bool declare_var = false;
    if (tok_is(vm, TK_K_LET)) { declare_var = true; tok_advance(vm); }

    if (!tok_is(vm, TK_IDENT)) {
        return comp_error_tok(vm, tok_cur(vm),
                              "expected loop variable after 'for'");
    }
    const VantaToken *id_tok = tok_cur(vm);
    int32_t ni = name_index(vm, vm->source + id_tok->src_off, id_tok->v.len);
    if (ni < 0) return comp_error_tok(vm, id_tok, "identifier table exhausted");
    tok_advance(vm);

    r = expect_tok(vm, TK_K_IN);
    if (r) return r;

    /* iterable expression */
    r = compile_expr(vm);
    if (r) return r;
    r = emit_op(vm, OP_ITER_BEGIN);
    if (r) return r;

    r = push_loop(vm, vm->cur_fn);
    if (r) return r;
    VantaLoop *loop = cur_loop(vm);
    loop->drop_slots = 0;   /* ITER_END at the end cleans the two slots */

    enter_scope(vm);
    uint32_t scope_entry = vm->scope_depth;

    uint32_t var_slot = VANTA_NO_INDEX;
    bool var_global = false;
    uint32_t var_gslot = VANTA_NO_INDEX;
    if (declare_var) {
        r = local_declare(vm, (uint32_t)ni, false, id_tok->line, id_tok->col, &var_slot);
        if (r) { exit_scope(vm, scope_entry); pop_loop(vm); return r; }
    } else {
        /* existing variable: must already be declared */
        ResolveOut res;
        if (!resolve_ident(vm, (uint32_t)ni, &res)) {
            r = comp_error_tok(vm, id_tok,
                               "unknown identifier '%s' (use 'let' to declare it here)",
                               vm->names[ni].name);
            exit_scope(vm, scope_entry); pop_loop(vm);
            return r;
        }
        if (res.is_const) {
            r = comp_error_tok(vm, id_tok, "cannot assign loop values to const '%s'",
                               vm->names[ni].name);
            exit_scope(vm, scope_entry); pop_loop(vm);
            return r;
        }
        if (res.kind == RESOLVE_LOCAL) var_slot = res.slot;
        else if (res.kind == RESOLVE_GLOBAL) { var_global = true; var_gslot = res.idx; }
        else {
            r = comp_error_tok(vm, id_tok, "for-in variable must be a variable");
            exit_scope(vm, scope_entry); pop_loop(vm);
            return r;
        }
    }

    uint32_t loop_top = bc_pos(vm);
    uint32_t end_patch;
    r = emit_jump(vm, OP_ITER_NEXT, &end_patch);
    if (r) { exit_scope(vm, scope_entry); pop_loop(vm); return r; }

    /* element on top of the iterator pair: store into the variable */
    if (!var_global) {
        r = emit_op_u32(vm, OP_STORE_LOCAL, var_slot);
    } else {
        r = emit_op_u32(vm, OP_STORE_GLOBAL, var_gslot);
    }
    if (r) { exit_scope(vm, scope_entry); pop_loop(vm); return r; }

    skip_nl(vm);
    r = compile_body(vm);
    if (r) { exit_scope(vm, scope_entry); pop_loop(vm); return r; }

    r = emit_op_u32(vm, OP_JMP, loop_top);
    if (r) { exit_scope(vm, scope_entry); pop_loop(vm); return r; }

    uint32_t end_pc = bc_pos(vm);
    patch_jump(vm, end_patch, end_pc);
    r = emit_op(vm, OP_ITER_END);
    if (r) { exit_scope(vm, scope_entry); pop_loop(vm); return r; }

    /* break exits through ITER_END so the iterator slots always die exactly
     * once; continue re-enters ITER_NEXT with the pair still on the stack */
    patch_list_resolve(vm, loop->break_patches, end_pc);
    patch_list_resolve(vm, loop->continue_patches, loop_top);
    pop_loop(vm);
    exit_scope(vm, scope_entry);
    return VANTA_OK;
}

static VantaResult compile_for(VantaVM *vm) {
    tok_advance(vm);   /* 'for' */
    skip_nl(vm);
    if (tok_is(vm, TK_K_LET)) {
        /* could be classic-for with let-initializer OR for-let-in:
         * lookahead for 'IDENT in' */
        if (vm->cur_token + 2 < vm->token_count &&
            vm->tokens[vm->cur_token + 1].kind == TK_IDENT &&
            vm->tokens[vm->cur_token + 2].kind == TK_K_IN) {
            return compile_for_in(vm);    /* re-consumes 'let' itself */
        }
        return compile_for_classic(vm, true);
    }
    if (tok_is(vm, TK_IDENT) && vm->cur_token + 1 < vm->token_count &&
        vm->tokens[vm->cur_token + 1].kind == TK_K_IN) {
        return compile_for_in(vm);
    }
    bool has_init = !tok_is(vm, TK_SEMI);
    return compile_for_classic(vm, has_init);
}

/* break / continue / return */

static VantaResult compile_break(VantaVM *vm) {
    const VantaToken *t = tok_cur(vm);
    tok_advance(vm);
    VantaLoop *loop = cur_loop(vm);
    if (!loop || loop->fn_idx != vm->cur_fn) {
        return comp_error_tok(vm, t, "'break' outside of a loop");
    }
    for (uint32_t i = 0; i < loop->drop_slots; i++) {
        VantaResult r = emit_op(vm, OP_POP);
        if (r) return r;
    }
    return patch_list_add(vm, OP_JMP, &loop->break_patches);
}

static VantaResult compile_continue(VantaVM *vm) {
    const VantaToken *t = tok_cur(vm);
    tok_advance(vm);
    VantaLoop *loop = cur_loop(vm);
    if (!loop || loop->fn_idx != vm->cur_fn) {
        return comp_error_tok(vm, t, "'continue' outside of a loop");
    }
    return patch_list_add(vm, OP_JMP, &loop->continue_patches);
}

static VantaResult compile_return(VantaVM *vm) {
    tok_advance(vm);   /* 'return' */
    /* bare return when the next token terminates a statement */
    if (tok_is(vm, TK_NL) || tok_is(vm, TK_SEMI) || tok_is(vm, TK_RBRACE) ||
        tok_is(vm, TK_EOF)) {
        return emit_op(vm, OP_RETURN_NIL);
    }
    VantaResult r = compile_expr(vm);
    if (r) return r;
    return emit_op(vm, OP_RETURN);
}

/* statement dispatch */

static VantaResult compile_statement(VantaVM *vm, bool top_level) {
    skip_nl(vm);
    const VantaToken *t = tok_cur(vm);
    note_source_line(vm, t->line);
    VantaResult r;

    switch (t->kind) {
        case TK_EOF:
            return VANTA_OK;

        case TK_NL:
            skip_nl(vm);
            return VANTA_OK;

        case TK_SEMI:
            tok_advance(vm);
            return VANTA_OK;

        case TK_K_LET:
            r = compile_let(vm, false);
            if (r) return r;
            return expect_terminator(vm);

        case TK_K_CONST:
            r = compile_let(vm, true);
            if (r) return r;
            return expect_terminator(vm);

        case TK_K_FN: {
            /* statement form: named functions store their value into the
             * global binding; an anonymous fn statement leaves its value
             * behind, so drop it to keep the stack balanced */
            bool named = (vm->cur_token + 1 < vm->token_count &&
                          vm->tokens[vm->cur_token + 1].kind == TK_IDENT);
            PrimaryInfo pi;
            r = compile_fn_expression(vm, t, &pi);
            if (r) return r;
            if (!named) {
                r = emit_op(vm, OP_POP);
                if (r) return r;
            }
            return expect_terminator(vm);
        }

        case TK_K_IF:
            r = compile_if(vm);
            if (r) return r;
            return VANTA_OK;   /* blocks carry their own termination */

        case TK_K_WHILE:
            r = compile_while(vm);
            if (r) return r;
            return VANTA_OK;

        case TK_K_FOR:
            r = compile_for(vm);
            if (r) return r;
            return VANTA_OK;

        case TK_K_BREAK:
            r = compile_break(vm);
            if (r) return r;
            return expect_terminator(vm);

        case TK_K_CONTINUE:
            r = compile_continue(vm);
            if (r) return r;
            return expect_terminator(vm);

        case TK_K_RETURN:
            r = compile_return(vm);
            if (r) return r;
            return expect_terminator(vm);

        case TK_LBRACE:
            return compile_block(vm);

        case TK_IDENT: {
            /* assignment scan: rewind on failure */
            AssignStep steps[ASSIGN_DEPTH_MAX];
            uint32_t step_count = 0;
            bool is_assign = false;
            uint32_t save_cursor = vm->cur_token;

            r = scan_assign_target(vm, steps, &step_count, &is_assign);
            if (r) return r;

            if (is_assign) {
                uint32_t op_kind = tok_cur(vm)->kind;
                tok_advance(vm);              /* consume the assignment op */
                skip_nl(vm);
                r = compile_assignment(vm, &vm->tokens[save_cursor], steps,
                                       step_count, op_kind);
                if (r) return r;
                return expect_terminator(vm);
            }

            vm->cur_token = save_cursor;
            r = compile_expr(vm);
            if (r) return r;
            r = emit_op(vm, OP_POP);          /* expression statements discard */
            if (r) return r;
            return expect_terminator(vm);
        }

        default:
            /* any other expression statement (calls start with a name, but
             * literal statements like `1` are legal and pointless) */
            r = compile_expr(vm);
            if (r) return r;
            r = emit_op(vm, OP_POP);
            if (r) return r;
            return expect_terminator(vm);
    }
}

/* top-level pre-scan
 * Function names at brace depth 0 are hoisted: `fn a(){ b() } fn b(){}`
 * compiles cleanly. Let/const are NOT hoisted - reading a binding before
 * its declaration line stays a compile error, which is what users expect
 * from a top-to-bottom shell language. */

static void prescan_hoist_functions(VantaVM *vm) {
    uint32_t depth = 0;
    for (uint32_t i = 0; i < vm->token_count; i++) {
        uint32_t k = vm->tokens[i].kind;
        if (k == TK_LBRACE) depth++;
        else if (k == TK_RBRACE && depth > 0) depth--;
        else if (k == TK_K_FN && depth == 0 && i + 1 < vm->token_count &&
                 vm->tokens[i + 1].kind == TK_IDENT) {
            const VantaToken *nt = &vm->tokens[i + 1];
            int32_t ni = name_index(vm, vm->source + nt->src_off, nt->v.len);
            if (ni >= 0) global_declare(vm, (uint32_t)ni, GLOBAL_FLAG_DEFINED);
            i++;   /* skip the name token */
        }
    }
}

/* program driver */

static VantaResult compile_program(VantaVM *vm) {
    VantaResult r = lex_program(vm);
    if (r != VANTA_OK) return r;

    /* compiler state reset: names/consts/globals survive vanta_init only;
     * function records and bytecode start fresh every run */
    vm->bc_len = 0;
    vm->cur_token = 0;
    vm->cur_fn = VANTA_NO_INDEX;   /* main script frame */
    vm->local_count = 0;
    vm->localdesc_count = 0;
    vm->scope_depth = 0;
    vm->loop_depth = 0;
    vm->lineinfo_count = 0;
    vm->last_line_emitted = 0;
    vm->function_count = 0;

    prescan_hoist_functions(vm);

    while (!tok_is(vm, TK_EOF)) {
        r = compile_statement(vm, true);
        if (r != VANTA_OK) return r;
        skip_nl(vm);
    }

    r = emit_op(vm, OP_HALT);
    if (r != VANTA_OK) return r;

    vm->compiled = 1;
    return VANTA_OK;
}

/* P16 - runtime prelude: errors, operand readers, stack discipline
 *
 * Every runtime trap funnels through rt_error: message, source line (from
 * the bytecode line map), and a stack trace built from live call frames.
 * The interpreter checks fuel on every dispatch so a runaway script burns
 * its budget and yields the shell back instead of hanging the kernel.
 *
 * Arithmetic wraps on int64 overflow (C two's-complement semantics),
 * matching every systems programmer's expectation; there is no arbitrary
 * precision escape hatch in a 500 KB VM, and pretending otherwise would
 * be worse than wrapping. */

extern int vanta_syscall_hook(int sys_id, int64_t arg1, int64_t arg2);

static VantaResult rt_error(VantaVM *vm, VantaResult code, const char *fmt, ...) {
    char msg[VANTA_DIAG_MSG];
    va_list ap;
    va_start(ap, fmt);
    int n = vanta_vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;

    VantaDiag *d = &vm->diag;
    d->result = code;
    d->line = pc_to_line(vm, vm->pc);
    d->col = 0;
    d->msg_len = (uint32_t)v_strlen(msg);
    v_strlcpy(d->msg, msg, sizeof(d->msg));

    /* stack trace: innermost frame first. The top frame's line is the
     * executing pc; caller frames report their return pc. */
    VantaTrace *t = &vm->trace;
    t->count = 0;
    uint32_t frames = vm->frame_count;
    for (uint32_t i = frames; i-- > 0 && t->count < VANTA_TRACE_MAX;) {
        VantaFrame *f = &vm->frames[i];
        uint32_t name_idx = VANTA_NO_INDEX;
        if (f->fn_idx != VANTA_NO_INDEX && f->fn_idx < vm->function_count) {
            name_idx = vm->functions[f->fn_idx].name_idx;
        }
        t->fn_name_idx[t->count] = name_idx;
        t->line[t->count] = (i + 1 == frames)
                            ? pc_to_line(vm, vm->pc)
                            : pc_to_line(vm, f->ret_pc);
        t->count++;
    }
    return code;
}

/* bytecode operand readers
 * Every reader validates the remaining opcode window against bc_len: a
 * corrupt or truncated code stream cannot walk past the end of the arena. */

static VantaResult read_u32(VantaVM *vm, uint32_t pc, uint32_t *out) {
    if (pc + 4 > vm->bc_len) {
        return rt_error(vm, VANTA_ERR_INTERNAL,
                        "bytecode overrun at pc=%u (u32 operand)", pc);
    }
    *out = (uint32_t)vm->bytecode[pc]
         | ((uint32_t)vm->bytecode[pc + 1] << 8)
         | ((uint32_t)vm->bytecode[pc + 2] << 16)
         | ((uint32_t)vm->bytecode[pc + 3] << 24);
    return VANTA_OK;
}

static VantaResult read_i64(VantaVM *vm, uint32_t pc, int64_t *out) {
    if (pc + 8 > vm->bc_len) {
        return rt_error(vm, VANTA_ERR_INTERNAL,
                        "bytecode overrun at pc=%u (i64 operand)", pc);
    }
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= (uint64_t)vm->bytecode[pc + i] << (i * 8);
    }
    *out = (int64_t)v;
    return VANTA_OK;
}

static VantaResult read_f64(VantaVM *vm, uint32_t pc, double *out) {
    if (pc + 8 > vm->bc_len) {
        return rt_error(vm, VANTA_ERR_INTERNAL,
                        "bytecode overrun at pc=%u (f64 operand)", pc);
    }
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= (uint64_t)vm->bytecode[pc + i] << (i * 8);
    }
    *out = bits_to_f64(v);
    return VANTA_OK;
}

/* value stack discipline
 * push_value transfers ownership: the caller's reference becomes the
 * stack's. Pop is the inverse. Every opcode balances; the compiler's stack
 * discipline plus these checks bounds everything. */

static VantaResult push_value(VantaVM *vm, VantaValue v) {
    if (vm->sp >= VANTA_MAX_STACK) {
        return rt_error(vm, VANTA_ERR_STACK_OVERFLOW,
                        "value stack exhausted (%u slots)", VANTA_MAX_STACK);
    }
    vm->stack[vm->sp++] = v;
    return VANTA_OK;
}

static VantaValue pop_value(VantaVM *vm) {
    return vm->stack[--vm->sp];
}

/* POP that releases the reference */
static VantaResult pop_discard(VantaVM *vm) {
    if (vm->sp == 0) {
        return rt_error(vm, VANTA_ERR_INTERNAL, "stack underflow on POP");
    }
    VantaValue v = pop_value(vm);
    value_release(vm, v);
    return VANTA_OK;
}

/* binary numeric pop: both ints -> int64 pair, any float -> double pair,
 * mixed promotes the int. Anything else is a typed error listing the actual
 * types, because "1 + \"a\"" deserves a message, not a crash. */
typedef enum { NUM_NONE = 0, NUM_BOTH_INT, NUM_ANY_FLOAT } NumericShape;

static NumericShape pop_two_numeric(VantaVM *vm, VantaValue *av, VantaValue *bv,
                                    int64_t *ai, int64_t *bi,
                                    double *af, double *bf, const char *what) {
    VantaValue b = vm->stack[--vm->sp];
    VantaValue a = vm->stack[--vm->sp];
    (void)bv;
    (void)av;

    bool an = (a.type == V_INT || a.type == V_FLOAT);
    bool bn = (b.type == V_INT || b.type == V_FLOAT);
    if (!an || !bn) {
        rt_error(vm, VANTA_ERR_TYPE, "%s requires numbers, got %s and %s",
                 what, value_type_name(a), value_type_name(b));
        return NUM_NONE;
    }
    if (a.type == V_INT && b.type == V_INT) {
        *ai = a.as.i;
        *bi = b.as.i;
        return NUM_BOTH_INT;
    }
    *af = (a.type == V_INT) ? (double)a.as.i : a.as.f;
    *bf = (b.type == V_INT) ? (double)b.as.i : b.as.f;
    return NUM_ANY_FLOAT;
}

/* int-only binary pop for bitwise operators */
static bool pop_two_int(VantaVM *vm, int64_t *ai, int64_t *bi, const char *what) {
    VantaValue b = vm->stack[--vm->sp];
    VantaValue a = vm->stack[--vm->sp];
    if (a.type != V_INT || b.type != V_INT) {
        return rt_error(vm, VANTA_ERR_TYPE, "%s requires integers, got %s and %s",
                        what, value_type_name(a), value_type_name(b)) == VANTA_OK;
    }
    *ai = a.as.i;
    *bi = b.as.i;
    return true;
}

/* call frames
 * Frames live in a bounded array. Argument transfer is ownership-neutral:
 * the caller's refs on the stack become the callee's local-slot refs
 * unchanged - no retain/release churn on every call. */

static VantaResult frame_push(VantaVM *vm, uint32_t fn_idx, uint32_t ret_pc,
                              uint32_t locals_base, uint32_t stack_base) {
    if (vm->frame_count >= VANTA_MAX_FRAMES) {
        return rt_error(vm, VANTA_ERR_STACK_OVERFLOW,
                        "call depth exceeded (%u frames)", VANTA_MAX_FRAMES);
    }
    VantaFrame *f = &vm->frames[vm->frame_count++];
    f->ret_pc = ret_pc;
    f->locals_base = locals_base;
    f->stack_base = stack_base;
    f->fn_idx = fn_idx;
    return VANTA_OK;
}

/* release every live local of a frame (return / abort paths share this) */
static void frame_release_locals(VantaVM *vm, const VantaFrame *f) {
    for (uint32_t i = f->locals_base; i < vm->locals_top; i++) {
        value_release(vm, vm->locals[i]);
        vm->locals[i] = VANTA_VALUE_NIL;
    }
}

/* interpreter: CALL machinery
 * Shared by OP_CALL: the callee value sits below the arguments on the
 * stack; natives run inline, script functions push a frame and jump. */

static VantaResult do_call(VantaVM *vm, uint32_t argc) {
    if (vm->sp < argc + 1) {
        return rt_error(vm, VANTA_ERR_INTERNAL, "CALL with corrupted stack");
    }
    VantaValue callee = vm->stack[vm->sp - argc - 1];
    VantaValue *args = &vm->stack[vm->sp - argc];

    if (callee.type == V_NATIVE) {
        uint32_t min_a, max_a;
        uint32_t id = (uint32_t)callee.as.i;
        if (id >= N_NATIVE_COUNT || !native_bounds(id, &min_a, &max_a)) {
            return rt_error(vm, VANTA_ERR_RUNTIME, "invalid native id %u", id);
        }
        if (argc < min_a || argc > max_a) {
            VantaResult er = rt_error(vm, VANTA_ERR_ARITY,
                                "native '%s' expects %u..%u arguments, got %u",
                                native_name(id), min_a, max_a, argc);
            /* release arguments: the callee path cleans the stack */
            for (uint32_t i = 0; i < argc; i++) value_release(vm, args[i]);
            vm->sp -= argc + 1;
            return er;
        }
        VantaValue out = VANTA_VALUE_NIL;
        VantaResult r = call_native(vm, id, argc, args, &out);
        /* arguments are consumed either way */
        for (uint32_t i = 0; i < argc; i++) value_release(vm, args[i]);
        if (r != VANTA_OK) {
            vm->sp -= argc + 1;
            if (vm->diag.result == VANTA_OK) vm->diag.result = r;
            return r;
        }
        vm->sp -= argc + 1;
        return push_value(vm, out);
    }

    if (callee.type == V_FUNC) {
        uint32_t idx = (uint32_t)callee.as.i;
        if (idx >= vm->function_count) {
            for (uint32_t i = 0; i < argc; i++) value_release(vm, args[i]);
            vm->sp -= argc + 1;
            return rt_error(vm, VANTA_ERR_RUNTIME, "invalid function index %u", idx);
        }
        VantaFunction *fn = &vm->functions[idx];
        if (argc != fn->nparams) {
            VantaResult er = rt_error(vm, VANTA_ERR_ARITY,
                                "function '%s' expects %u arguments, got %u",
                                (fn->name_idx != VANTA_NO_INDEX)
                                    ? vm->names[fn->name_idx].name : "<anonymous>",
                                fn->nparams, argc);
            for (uint32_t i = 0; i < argc; i++) value_release(vm, args[i]);
            vm->sp -= argc + 1;
            return er;
        }
        if (vm->locals_top + fn->nlocals > VANTA_MAX_LOCALS) {
            for (uint32_t i = 0; i < argc; i++) value_release(vm, args[i]);
            vm->sp -= argc + 1;
            return rt_error(vm, VANTA_ERR_STACK_OVERFLOW,
                            "local slot space exhausted (%u)", VANTA_MAX_LOCALS);
        }

        uint32_t stack_base = vm->sp - argc - 1;   /* below callee + args */
        VantaResult r = frame_push(vm, idx, vm->pc, vm->locals_top, stack_base);
        if (r != VANTA_OK) {
            for (uint32_t i = 0; i < argc; i++) value_release(vm, args[i]);
            vm->sp -= argc + 1;
            return r;
        }

        /* transfer args into the fresh locals, zero the rest */
        uint32_t base = vm->locals_top;
        for (uint32_t i = 0; i < fn->nlocals; i++) {
            if (i < argc) vm->locals[base + i] = args[i];
            else          vm->locals[base + i] = VANTA_VALUE_NIL;
        }
        vm->locals_top = base + fn->nlocals;
        vm->sp = stack_base;                       /* drop callee + args */
        vm->pc = fn->code_off;
        return VANTA_OK;
    }

    for (uint32_t i = 0; i < argc; i++) value_release(vm, args[i]);
    vm->sp -= argc + 1;
    return rt_error(vm, VANTA_ERR_TYPE, "value of type %s is not callable",
                    value_type_name(callee));
}

/* RETURN shared by OP_RETURN and OP_RETURN_NIL */
static VantaResult do_return(VantaVM *vm, VantaValue result) {
    if (vm->frame_count == 0) {
        value_release(vm, result);
        return rt_error(vm, VANTA_ERR_INTERNAL, "RETURN without a frame");
    }
    VantaFrame *f = &vm->frames[vm->frame_count - 1];

    if (f->fn_idx == VANTA_NO_INDEX) {
        /* main script frame: returning ends the program */
        frame_release_locals(vm, f);
        vm->frame_count--;
        vm->locals_top = f->locals_base;
        vm->sp = f->stack_base;
        value_release(vm, result);
        vm->pc = vm->bc_len;                       /* exit the dispatch loop */
        return VANTA_OK;
    }

    frame_release_locals(vm, f);
    vm->locals_top = f->locals_base;
    vm->sp = f->stack_base;                        /* drop callee + locals view */
    vm->pc = f->ret_pc;
    vm->frame_count--;
    return push_value(vm, result);
}

/* P17 - native surface and collection methods */

static void arr_release(VantaVM *vm, uint32_t handle) {
    value_release(vm, make_arr(handle));
}

static void map_release(VantaVM *vm, uint32_t handle) {
    value_release(vm, make_map(handle));
}

typedef struct {
    const char *name;
    uint8_t min_args;
    uint8_t max_args;
} NativeInfo;

static const NativeInfo native_table[N_NATIVE_COUNT] = {
    [N_PRINT]      = { "print", 0, VANTA_MAX_ARGS },
    [N_PRINTLN]    = { "println", 0, VANTA_MAX_ARGS },
    [N_DBG]        = { "dbg", 1, VANTA_MAX_ARGS },
    [N_FORMAT]     = { "format", 1, VANTA_MAX_ARGS },
    [N_LEN]        = { "len", 1, 1 }, [N_TYPE] = { "type", 1, 1 },
    [N_STR]        = { "str", 1, 1 }, [N_INT] = { "int", 1, 1 },
    [N_FLOAT]      = { "float", 1, 1 }, [N_BOOL] = { "bool", 1, 1 },
    [N_ABS]        = { "abs", 1, 1 }, [N_SIGN] = { "sign", 1, 1 },
    [N_MIN]        = { "min", 2, VANTA_MAX_ARGS },
    [N_MAX]        = { "max", 2, VANTA_MAX_ARGS },
    [N_CLAMP]      = { "clamp", 3, 3 }, [N_GCD] = { "gcd", 2, 2 },
    [N_LCM]        = { "lcm", 2, 2 }, [N_SQRT] = { "sqrt", 1, 1 },
    [N_CBRT]       = { "cbrt", 1, 1 }, [N_POW] = { "pow", 2, 2 },
    [N_EXP]        = { "exp", 1, 1 }, [N_LOG] = { "log", 1, 1 },
    [N_LOG10]      = { "log10", 1, 1 }, [N_FLOOR] = { "floor", 1, 1 },
    [N_CEIL]       = { "ceil", 1, 1 }, [N_ROUND] = { "round", 1, 1 },
    [N_TRUNC]      = { "trunc", 1, 1 }, [N_FMOD] = { "fmod", 2, 2 },
    [N_HYPOT]      = { "hypot", 2, 2 }, [N_SIN] = { "sin", 1, 1 },
    [N_COS]        = { "cos", 1, 1 }, [N_TAN] = { "tan", 1, 1 },
    [N_ASIN]       = { "asin", 1, 1 }, [N_ACOS] = { "acos", 1, 1 },
    [N_ATAN]       = { "atan", 1, 1 }, [N_ATAN2] = { "atan2", 2, 2 },
    [N_SINH]       = { "sinh", 1, 1 }, [N_COSH] = { "cosh", 1, 1 },
    [N_TANH]       = { "tanh", 1, 1 },
    [N_ISNAN]      = { "isNaN", 1, 1 }, [N_ISINF] = { "isInf", 1, 1 },
    [N_ISINT]      = { "isInt", 1, 1 }, [N_ISFLOAT] = { "isFloat", 1, 1 },
    [N_ISSTR]      = { "isStr", 1, 1 }, [N_ISARR] = { "isArr", 1, 1 },
    [N_ISMAP]      = { "isMap", 1, 1 }, [N_ISFN] = { "isFn", 1, 1 },
    [N_RAND]       = { "rand", 0, 0 }, [N_RANDINT] = { "randInt", 2, 2 },
    [N_RANDSEED]   = { "randSeed", 1, 1 }, [N_RANDCHOICE] = { "randChoice", 1, 1 },
    [N_ARRAY]      = { "array", 0, VANTA_MAX_ARGS },
    [N_ARRAYOF]    = { "arrayOf", 2, 2 }, [N_MAP] = { "map", 0, 0 },
    [N_RANGE]      = { "range", 1, 3 }, [N_SORT] = { "sort", 1, 1 },
    [N_REVERSE]    = { "reverse", 1, 1 }, [N_HASH] = { "hash", 1, 1 },
    [N_CHR]        = { "chr", 1, 1 }, [N_ORD] = { "ord", 1, 1 },
    [N_SYSCALL]    = { "syscall", 1, 3 }, [N_ASSERT] = { "assert", 1, 2 },
    [N_EXIT]       = { "exit", 0, 1 }, [N_CLOCK] = { "clock", 0, 0 }
};

static const char *native_name(uint32_t id) {
    return id < N_NATIVE_COUNT ? native_table[id].name : "?native";
}

static bool native_bounds(uint32_t id, uint32_t *min_a, uint32_t *max_a) {
    if (id >= N_NATIVE_COUNT || !native_table[id].name) return false;
    *min_a = native_table[id].min_args;
    *max_a = native_table[id].max_args;
    return true;
}

static int32_t builtin_lookup_id(VantaVM *vm, const char *s, uint32_t len) {
    (void)vm;
    for (uint32_t i = 0; i < N_NATIVE_COUNT; i++) {
        const char *name = native_table[i].name;
        if (!name) continue;
        uint32_t nlen = (uint32_t)v_strlen(name);
        if (nlen == len && v_memcmp_eq(name, s, len)) return (int32_t)i;
    }
    return -1;
}

static VantaResult native_oom(VantaVM *vm) {
    return rt_error(vm, VANTA_ERR_OUT_OF_MEMORY, "Vanta bounded pool exhausted");
}

static bool value_as_int(VantaVM *vm, VantaValue v, int64_t *out, const char *where) {
    if (v.type != V_INT) {
        rt_error(vm, VANTA_ERR_TYPE, "%s requires an integer, got %s", where, value_type_name(v));
        return false;
    }
    *out = v.as.i;
    return true;
}

static bool value_as_number(VantaVM *vm, VantaValue v, double *out, const char *where) {
    if (v.type == V_INT) {
        *out = (double)v.as.i;
        return true;
    }
    if (v.type == V_FLOAT) {
        *out = v.as.f;
        return true;
    }
    rt_error(vm, VANTA_ERR_TYPE, "%s requires a number, got %s", where, value_type_name(v));
    return false;
}

static VantaResult value_to_new_string(VantaVM *vm, VantaValue v, VantaValue *out) {
    sb_reset(&vm->sb);
    value_to_sb(vm, &vm->sb, v);
    uint32_t h = str_take_sb(vm, &vm->sb);
    if (h == VANTA_NIL_HANDLE) return native_oom(vm);
    *out = make_str(h);
    return VANTA_OK;
}

static VantaResult make_string_bytes(VantaVM *vm, const char *bytes, uint32_t len,
                                     VantaValue *out) {
    uint32_t h = str_alloc(vm, bytes, len);
    if (h == VANTA_NIL_HANDLE) return native_oom(vm);
    *out = make_str(h);
    return VANTA_OK;
}

static VantaResult native_numeric_unary(VantaVM *vm, VantaValue arg,
                                        double (*fn)(double), VantaValue *out,
                                        const char *name) {
    double x;
    if (!value_as_number(vm, arg, &x, name)) return vm->diag.result;
    *out = make_float(fn(x));
    return VANTA_OK;
}

static int64_t gcd_i64(int64_t a, int64_t b) {
    uint64_t ua = a < 0 ? (uint64_t)(0 - (uint64_t)a) : (uint64_t)a;
    uint64_t ub = b < 0 ? (uint64_t)(0 - (uint64_t)b) : (uint64_t)b;
    while (ub != 0) {
        uint64_t rem = ua % ub;
        ua = ub;
        ub = rem;
    }
    return ua > 0x7FFFFFFFFFFFFFFFULL ? (-9223372036854775807LL - 1LL) : (int64_t)ua;
}

static VantaResult map_set_value(VantaVM *vm, VantaValue recv, VantaValue key,
                                 VantaValue val, VantaValue *out) {
    if (recv.type != V_MAP || key.type != V_STR) {
        return rt_error(vm, VANTA_ERR_TYPE, "map.set requires a map and a string key");
    }
    if (!map_set(vm, recv.as.h, key.as.h, val)) return native_oom(vm);
    value_retain(vm, val);
    *out = val;
    return VANTA_OK;
}

static VantaResult string_case(VantaVM *vm, VantaValue recv, bool upper, VantaValue *out) {
    if (recv.type != V_STR) return rt_error(vm, VANTA_ERR_TYPE, "string method requires a string");
    uint32_t n = str_len(vm, recv.as.h);
    const char *s = str_data(vm, recv.as.h);
    sb_reset(&vm->sb);
    for (uint32_t i = 0; i < n; i++) sb_append_char(&vm->sb, upper ? upper_ascii(s[i]) : lower_ascii(s[i]));
    uint32_t h = str_take_sb(vm, &vm->sb);
    if (h == VANTA_NIL_HANDLE) return native_oom(vm);
    *out = make_str(h);
    return VANTA_OK;
}

static VantaResult call_method(VantaVM *vm, uint32_t method, VantaValue recv,
                               uint32_t argc, VantaValue *argv, VantaValue *out) {
    *out = VANTA_VALUE_NIL;
    if (method >= M_METHOD_COUNT) return rt_error(vm, VANTA_ERR_RUNTIME, "unknown method id %u", method);

    if (method == M_LEN) {
        if (argc != 0) return rt_error(vm, VANTA_ERR_ARITY, "len method takes no arguments");
        if (recv.type == V_STR) *out = make_int((int64_t)utf8_length(str_data(vm, recv.as.h), str_len(vm, recv.as.h)));
        else if (recv.type == V_ARR) *out = make_int((int64_t)arr_len(vm, recv.as.h));
        else if (recv.type == V_MAP) *out = make_int((int64_t)map_count(vm, recv.as.h));
        else return rt_error(vm, VANTA_ERR_TYPE, "len is not defined for %s", value_type_name(recv));
        return VANTA_OK;
    }

    switch (method) {
        case M_PUSH:
            if (recv.type != V_ARR || argc != 1) return rt_error(vm, VANTA_ERR_ARITY, "array.push expects one argument");
            if (!arr_push(vm, recv.as.h, argv[0])) return native_oom(vm);
            *out = make_int((int64_t)arr_len(vm, recv.as.h));
            return VANTA_OK;
        case M_POP:
            if (recv.type != V_ARR || argc != 0) return rt_error(vm, VANTA_ERR_ARITY, "array.pop takes no arguments");
            if (!arr_pop(vm, recv.as.h, out)) *out = VANTA_VALUE_NIL;
            return VANTA_OK;
        case M_SHIFT:
            if (recv.type != V_ARR || argc != 0) return rt_error(vm, VANTA_ERR_ARITY, "array.shift takes no arguments");
            if (!arr_remove_at(vm, recv.as.h, 0, out)) *out = VANTA_VALUE_NIL;
            return VANTA_OK;
        case M_UNSHIFT:
            if (recv.type != V_ARR || argc != 1) return rt_error(vm, VANTA_ERR_ARITY, "array.unshift expects one argument");
            if (!arr_insert(vm, recv.as.h, 0, argv[0])) return native_oom(vm);
            *out = make_int((int64_t)arr_len(vm, recv.as.h));
            return VANTA_OK;
        case M_INSERT: {
            int64_t idx;
            if (recv.type != V_ARR || argc != 2 || !value_as_int(vm, argv[0], &idx, "array.insert")) return vm->diag.result;
            if (idx < 0 || (uint64_t)idx > arr_len(vm, recv.as.h)) return rt_error(vm, VANTA_ERR_INDEX_BOUNDS, "array.insert index out of range");
            if (!arr_insert(vm, recv.as.h, (uint32_t)idx, argv[1])) return native_oom(vm);
            *out = make_int((int64_t)arr_len(vm, recv.as.h));
            return VANTA_OK;
        }
        case M_REMOVE: {
            int64_t idx;
            if (recv.type != V_ARR || argc != 1 || !value_as_int(vm, argv[0], &idx, "array.remove")) return vm->diag.result;
            if (idx < 0 || !arr_remove_at(vm, recv.as.h, (uint32_t)idx, out)) return rt_error(vm, VANTA_ERR_INDEX_BOUNDS, "array.remove index out of range");
            return VANTA_OK;
        }
        case M_GET:
            if (argc != 1) return rt_error(vm, VANTA_ERR_ARITY, "get expects one key");
            if (recv.type == V_MAP) {
                bool found;
                if (argv[0].type != V_STR) return rt_error(vm, VANTA_ERR_TYPE, "map.get key must be a string");
                if (!map_get(vm, recv.as.h, argv[0].as.h, out, &found)) return rt_error(vm, VANTA_ERR_RUNTIME, "invalid map");
                return VANTA_OK;
            }
            if (recv.type == V_ARR) {
                int64_t idx;
                if (!value_as_int(vm, argv[0], &idx, "array.get")) return vm->diag.result;
                if (idx < 0 || !arr_get(vm, recv.as.h, (uint32_t)idx, out)) return VANTA_OK;
                return VANTA_OK;
            }
            return rt_error(vm, VANTA_ERR_TYPE, "get is not defined for %s", value_type_name(recv));
        case M_SET:
            if (argc != 2) return rt_error(vm, VANTA_ERR_ARITY, "set expects key and value");
            if (recv.type == V_MAP) return map_set_value(vm, recv, argv[0], argv[1], out);
            if (recv.type == V_ARR) {
                int64_t idx;
                if (!value_as_int(vm, argv[0], &idx, "array.set")) return vm->diag.result;
                if (idx < 0 || !arr_set(vm, recv.as.h, (uint32_t)idx, argv[1])) return rt_error(vm, VANTA_ERR_INDEX_BOUNDS, "array.set index out of range");
                value_retain(vm, argv[1]);
                *out = argv[1];
                return VANTA_OK;
            }
            return rt_error(vm, VANTA_ERR_TYPE, "set is not defined for %s", value_type_name(recv));
        case M_HAS:
            if (recv.type != V_MAP || argc != 1 || argv[0].type != V_STR) return rt_error(vm, VANTA_ERR_TYPE, "map.has requires a string key");
            *out = make_bool(map_has(vm, recv.as.h, argv[0].as.h));
            return VANTA_OK;
        case M_DELETE: {
            bool found;
            if (recv.type != V_MAP || argc != 1 || argv[0].type != V_STR) return rt_error(vm, VANTA_ERR_TYPE, "map.delete requires a string key");
            if (!map_delete(vm, recv.as.h, argv[0].as.h, &found, NULL)) return rt_error(vm, VANTA_ERR_RUNTIME, "invalid map");
            *out = make_bool(found);
            return VANTA_OK;
        }
        case M_KEYS:
            if (recv.type != V_MAP || argc != 0) return rt_error(vm, VANTA_ERR_TYPE, "map.keys takes no arguments");
            { uint32_t h = map_keys_array(vm, recv.as.h); if (h == VANTA_NIL_HANDLE) return native_oom(vm); *out = make_arr(h); return VANTA_OK; }
        case M_VALUES:
            if (recv.type != V_MAP || argc != 0) return rt_error(vm, VANTA_ERR_TYPE, "map.values takes no arguments");
            { uint32_t h = map_values_array(vm, recv.as.h); if (h == VANTA_NIL_HANDLE) return native_oom(vm); *out = make_arr(h); return VANTA_OK; }
        case M_CLEAR:
            if (recv.type == V_MAP && argc == 0) { map_clear(vm, recv.as.h); return VANTA_OK; }
            if (recv.type == V_ARR && argc == 0) { while (arr_len(vm, recv.as.h)) { VantaValue discarded; arr_pop(vm, recv.as.h, &discarded); value_release(vm, discarded); } return VANTA_OK; }
            return rt_error(vm, VANTA_ERR_TYPE, "clear is defined for arrays and maps");
        case M_JOIN:
            if (recv.type != V_ARR || argc > 1 || (argc == 1 && argv[0].type != V_STR)) return rt_error(vm, VANTA_ERR_TYPE, "array.join takes an optional string separator");
            { uint32_t empty = argc ? argv[0].as.h : str_alloc(vm, "", 0); uint32_t h = arr_join(vm, recv.as.h, empty); if (!argc) str_release(vm, empty); if (h == VANTA_NIL_HANDLE) return native_oom(vm); *out = make_str(h); return VANTA_OK; }
        case M_SORT:
            if (recv.type != V_ARR || argc != 0) return rt_error(vm, VANTA_ERR_TYPE, "array.sort takes no arguments");
            arr_sort(vm, recv.as.h); value_retain(vm, recv); *out = recv; return VANTA_OK;
        case M_REVERSE:
            if (recv.type != V_ARR || argc != 0) return rt_error(vm, VANTA_ERR_TYPE, "array.reverse takes no arguments");
            arr_reverse(vm, recv.as.h); value_retain(vm, recv); *out = recv; return VANTA_OK;
        case M_FILL: {
            int64_t from = 0, to;
            if (recv.type != V_ARR || (argc != 1 && argc != 2 && argc != 3)) return rt_error(vm, VANTA_ERR_ARITY, "array.fill expects value, optional start and end");
            to = (int64_t)arr_len(vm, recv.as.h);
            if (argc >= 2 && !value_as_int(vm, argv[1], &from, "array.fill")) return vm->diag.result;
            if (argc == 3 && !value_as_int(vm, argv[2], &to, "array.fill")) return vm->diag.result;
            if (from < 0) from = 0; if (to < from) to = from;
            if ((uint64_t)to > arr_len(vm, recv.as.h)) to = (int64_t)arr_len(vm, recv.as.h);
            if (!arr_fill(vm, recv.as.h, (uint32_t)from, (uint32_t)to, argv[0])) return native_oom(vm);
            value_retain(vm, recv); *out = recv; return VANTA_OK;
        }
        case M_CONCAT:
            if (recv.type == V_ARR && argc == 1 && argv[0].type == V_ARR) {
                uint32_t h = arr_concat(vm, recv.as.h, argv[0].as.h); if (h == VANTA_NIL_HANDLE) return native_oom(vm); *out = make_arr(h); return VANTA_OK;
            }
            if (recv.type == V_STR && argc == 1) {
                sb_reset(&vm->sb); value_to_sb(vm, &vm->sb, recv); value_to_sb(vm, &vm->sb, argv[0]); uint32_t h = str_take_sb(vm, &vm->sb); if (h == VANTA_NIL_HANDLE) return native_oom(vm); *out = make_str(h); return VANTA_OK;
            }
            return rt_error(vm, VANTA_ERR_TYPE, "concat requires array+array or string+value");
        case M_SLICE: {
            int64_t from, to;
            if (argc != 2 || !value_as_int(vm, argv[0], &from, "slice") || !value_as_int(vm, argv[1], &to, "slice")) return vm->diag.result;
            if (recv.type == V_ARR) { bool ok; uint32_t h = arr_slice(vm, recv.as.h, from, to, &ok); if (!ok) return rt_error(vm, VANTA_ERR_INDEX_BOUNDS, "array.slice range invalid"); *out = make_arr(h); return VANTA_OK; }
            if (recv.type == V_STR) {
                uint32_t count = utf8_length(str_data(vm, recv.as.h), str_len(vm, recv.as.h));
                if (from < 0) from += count; if (to < 0) to += count;
                if (from < 0) from = 0; if (to < from) to = from; if ((uint64_t)to > count) to = count;
                uint32_t start = utf8_byte_offset(str_data(vm, recv.as.h), str_len(vm, recv.as.h), (uint32_t)from);
                uint32_t end = utf8_byte_offset(str_data(vm, recv.as.h), str_len(vm, recv.as.h), (uint32_t)to);
                uint32_t h = str_sub(vm, recv.as.h, start, end - start); if (h == VANTA_NIL_HANDLE) return native_oom(vm); *out = make_str(h); return VANTA_OK;
            }
            return rt_error(vm, VANTA_ERR_TYPE, "slice is defined for arrays and strings");
        }
        case M_INDEXOF:
            if (recv.type != V_ARR || argc != 1) return rt_error(vm, VANTA_ERR_TYPE, "array.indexOf expects one value");
            { int64_t pos; if (!arr_index_of(vm, recv.as.h, argv[0], &pos)) return rt_error(vm, VANTA_ERR_RUNTIME, "invalid array"); *out = make_int(pos); return VANTA_OK; }
        case M_CONTAINS:
            if (recv.type == V_ARR && argc == 1) { int64_t pos; arr_index_of(vm, recv.as.h, argv[0], &pos); *out = make_bool(pos >= 0); return VANTA_OK; }
            if (recv.type == V_STR && argc == 1 && argv[0].type == V_STR) { *out = make_bool(v_memmem(str_data(vm, recv.as.h), str_len(vm, recv.as.h), str_data(vm, argv[0].as.h), str_len(vm, argv[0].as.h)) != NULL); return VANTA_OK; }
            return rt_error(vm, VANTA_ERR_TYPE, "contains requires array/value or string/string");
        case M_UPPER: if (argc != 0) return rt_error(vm, VANTA_ERR_ARITY, "upper takes no arguments"); return string_case(vm, recv, true, out);
        case M_LOWER: if (argc != 0) return rt_error(vm, VANTA_ERR_ARITY, "lower takes no arguments"); return string_case(vm, recv, false, out);
        case M_TRIM: {
            if (recv.type != V_STR || argc != 0) return rt_error(vm, VANTA_ERR_TYPE, "string.trim takes no arguments");
            const char *s = str_data(vm, recv.as.h); uint32_t n = str_len(vm, recv.as.h), begin = 0, end = n;
            while (begin < end && is_space_char(s[begin])) begin++;
            while (end > begin && is_space_char(s[end - 1])) end--;
            return make_string_bytes(vm, s + begin, end - begin, out);
        }
        case M_SUBSTR: {
            int64_t start, count;
            if (recv.type != V_STR || (argc != 1 && argc != 2) || !value_as_int(vm, argv[0], &start, "string.substr")) return vm->diag.result;
            uint32_t ncp = utf8_length(str_data(vm, recv.as.h), str_len(vm, recv.as.h));
            if (start < 0) start += ncp; if (start < 0) start = 0; if ((uint64_t)start > ncp) start = ncp;
            count = argc == 2 ? 0 : (int64_t)ncp - start;
            if (argc == 2 && !value_as_int(vm, argv[1], &count, "string.substr")) return vm->diag.result;
            if (count < 0) count = 0; if ((uint64_t)count > ncp - (uint32_t)start) count = ncp - (uint32_t)start;
            uint32_t off = utf8_byte_offset(str_data(vm, recv.as.h), str_len(vm, recv.as.h), (uint32_t)start);
            uint32_t end = utf8_byte_offset(str_data(vm, recv.as.h), str_len(vm, recv.as.h), (uint32_t)start + (uint32_t)count);
            return make_string_bytes(vm, str_data(vm, recv.as.h) + off, end - off, out);
        }
        case M_STARTSWITH:
        case M_ENDSWITH: {
            if (recv.type != V_STR || argc != 1 || argv[0].type != V_STR) return rt_error(vm, VANTA_ERR_TYPE, "string prefix/suffix requires a string");
            uint32_t n = str_len(vm, recv.as.h), q = str_len(vm, argv[0].as.h);
            bool hit = q <= n && (method == M_STARTSWITH
                ? v_memcmp_eq(str_data(vm, recv.as.h), str_data(vm, argv[0].as.h), q)
                : v_memcmp_eq(str_data(vm, recv.as.h) + n - q, str_data(vm, argv[0].as.h), q));
            *out = make_bool(hit); return VANTA_OK;
        }
        case M_CHARAT: {
            int64_t idx;
            if (recv.type != V_STR || argc != 1 || !value_as_int(vm, argv[0], &idx, "string.charAt")) return vm->diag.result;
            uint32_t ncp = utf8_length(str_data(vm, recv.as.h), str_len(vm, recv.as.h));
            if (idx < 0 || (uint64_t)idx >= ncp) { *out = VANTA_VALUE_NIL; return VANTA_OK; }
            uint32_t off = utf8_byte_offset(str_data(vm, recv.as.h), str_len(vm, recv.as.h), (uint32_t)idx), cp;
            uint32_t bytes = utf8_decode(str_data(vm, recv.as.h) + off, str_len(vm, recv.as.h) - off, &cp);
            return make_string_bytes(vm, str_data(vm, recv.as.h) + off, bytes, out);
        }
        case M_CODEAT: {
            VantaValue char_out;
            VantaResult r = call_method(vm, M_CHARAT, recv, argc, argv, &char_out);
            if (r != VANTA_OK || char_out.type == V_NIL) { *out = char_out; return r; }
            uint32_t cp; utf8_decode(str_data(vm, char_out.as.h), str_len(vm, char_out.as.h), &cp); value_release(vm, char_out); *out = make_int((int64_t)cp); return VANTA_OK;
        }
        case M_FIND:
            if (recv.type != V_STR || argc != 1 || argv[0].type != V_STR) return rt_error(vm, VANTA_ERR_TYPE, "string.find requires a string");
            { const char *hit = v_memmem(str_data(vm, recv.as.h), str_len(vm, recv.as.h), str_data(vm, argv[0].as.h), str_len(vm, argv[0].as.h)); *out = make_int(hit ? (int64_t)utf8_length(str_data(vm, recv.as.h), (uint32_t)(hit - str_data(vm, recv.as.h))) : -1); return VANTA_OK; }
        case M_REPEAT: {
            int64_t times;
            if (recv.type != V_STR || argc != 1 || !value_as_int(vm, argv[0], &times, "string.repeat")) return vm->diag.result;
            if (times < 0) return rt_error(vm, VANTA_ERR_INDEX_BOUNDS, "string.repeat count must be non-negative");
            sb_reset(&vm->sb); for (int64_t i = 0; i < times; i++) if (!sb_append(&vm->sb, str_data(vm, recv.as.h), str_len(vm, recv.as.h))) return native_oom(vm);
            uint32_t h = str_take_sb(vm, &vm->sb); if (h == VANTA_NIL_HANDLE) return native_oom(vm); *out = make_str(h); return VANTA_OK;
        }
        default:
            return rt_error(vm, VANTA_ERR_RUNTIME, "method '%s' is not implemented for %s", vanta_method_name(method), value_type_name(recv));
    }
}

static VantaResult call_native(VantaVM *vm, uint32_t id, uint32_t argc,
                               VantaValue *argv, VantaValue *out) {
    *out = VANTA_VALUE_NIL;
    if (id >= N_NATIVE_COUNT) return rt_error(vm, VANTA_ERR_RUNTIME, "invalid native id");

    if (id == N_PRINT || id == N_PRINTLN || id == N_DBG) {
        sb_reset(&vm->sb);
        if (id == N_DBG) sb_append(&vm->sb, "[dbg] ", 6);
        for (uint32_t i = 0; i < argc; i++) {
            if (i) sb_append_char(&vm->sb, ' ');
            value_to_sb(vm, &vm->sb, argv[i]);
        }
        if (id != N_PRINT) sb_append_char(&vm->sb, '\n');
        vanta_out(vm, vm->sb.space);
        return VANTA_OK;
    }
    if (id == N_FORMAT) {
        if (argv[0].type != V_STR) return rt_error(vm, VANTA_ERR_TYPE, "format requires a string template");
        const char *fmt = str_data(vm, argv[0].as.h); uint32_t n = str_len(vm, argv[0].as.h), ai = 1;
        sb_reset(&vm->sb);
        for (uint32_t i = 0; i < n; i++) {
            if (fmt[i] == '{' && i + 1 < n && fmt[i + 1] == '}') { if (ai < argc) value_to_sb(vm, &vm->sb, argv[ai++]); else sb_append(&vm->sb, "{}", 2); i++; }
            else if (fmt[i] == '{' && i + 1 < n && fmt[i + 1] == '{') { sb_append_char(&vm->sb, '{'); i++; }
            else if (fmt[i] == '}' && i + 1 < n && fmt[i + 1] == '}') { sb_append_char(&vm->sb, '}'); i++; }
            else sb_append_char(&vm->sb, fmt[i]);
        }
        uint32_t h = str_take_sb(vm, &vm->sb); if (h == VANTA_NIL_HANDLE) return native_oom(vm); *out = make_str(h); return VANTA_OK;
    }
    if (id == N_LEN) {
        VantaValue a = argv[0];
        if (a.type == V_STR) *out = make_int((int64_t)utf8_length(str_data(vm, a.as.h), str_len(vm, a.as.h)));
        else if (a.type == V_ARR) *out = make_int((int64_t)arr_len(vm, a.as.h));
        else if (a.type == V_MAP) *out = make_int((int64_t)map_count(vm, a.as.h));
        else return rt_error(vm, VANTA_ERR_TYPE, "len is not defined for %s", value_type_name(a));
        return VANTA_OK;
    }
    if (id == N_TYPE) return make_string_bytes(vm, value_type_name(argv[0]), (uint32_t)v_strlen(value_type_name(argv[0])), out);
    if (id == N_STR) return value_to_new_string(vm, argv[0], out);
    if (id == N_BOOL) { *out = make_bool(value_truthy(vm, argv[0])); return VANTA_OK; }
    if (id == N_INT || id == N_FLOAT) {
        VantaValue a = argv[0];
        if (a.type == V_INT) { *out = id == N_INT ? a : make_float((double)a.as.i); return VANTA_OK; }
        if (a.type == V_FLOAT) { *out = id == N_INT ? make_int((int64_t)m_trunc(a.as.f)) : a; return VANTA_OK; }
        if (a.type == V_BOOL) { *out = id == N_INT ? make_int(a.as.i) : make_float((double)a.as.i); return VANTA_OK; }
        if (a.type == V_STR) {
            bool ok; if (id == N_INT) *out = make_int(v_parse_i64(str_data(vm, a.as.h), str_len(vm, a.as.h), 10, &ok));
            else *out = make_float(v_parse_f64(str_data(vm, a.as.h), str_len(vm, a.as.h), &ok));
            if (!ok) return rt_error(vm, VANTA_ERR_TYPE, "cannot parse %s as %s", str_data(vm, a.as.h), id == N_INT ? "int" : "float");
            return VANTA_OK;
        }
        return rt_error(vm, VANTA_ERR_TYPE, "cannot convert %s to %s", value_type_name(a), id == N_INT ? "int" : "float");
    }
    if (id == N_ABS || id == N_SIGN) {
        double x; if (!value_as_number(vm, argv[0], &x, native_name(id))) return vm->diag.result;
        if (id == N_ABS) *out = argv[0].type == V_INT ? make_int(argv[0].as.i < 0 ? (int64_t)(0 - (uint64_t)argv[0].as.i) : argv[0].as.i) : make_float(m_fabs(x));
        else *out = make_int(x > 0.0 ? 1 : (x < 0.0 ? -1 : 0));
        return VANTA_OK;
    }
    if (id == N_MIN || id == N_MAX) {
        VantaValue best = argv[0];
        for (uint32_t i = 1; i < argc; i++) { bool ok; int c = value_compare(vm, argv[i], best, &ok); if (!ok) return rt_error(vm, VANTA_ERR_TYPE, "%s requires comparable values", native_name(id)); if ((id == N_MIN && c < 0) || (id == N_MAX && c > 0)) best = argv[i]; }
        value_retain(vm, best); *out = best; return VANTA_OK;
    }
    if (id == N_CLAMP) {
        double v, lo, hi; if (!value_as_number(vm, argv[0], &v, "clamp") || !value_as_number(vm, argv[1], &lo, "clamp") || !value_as_number(vm, argv[2], &hi, "clamp")) return vm->diag.result;
        if (lo > hi) return rt_error(vm, VANTA_ERR_RUNTIME, "clamp lower bound exceeds upper bound");
        *out = make_float(v < lo ? lo : (v > hi ? hi : v)); return VANTA_OK;
    }
    if (id == N_GCD || id == N_LCM) {
        int64_t a,b; if (!value_as_int(vm, argv[0], &a, native_name(id)) || !value_as_int(vm, argv[1], &b, native_name(id))) return vm->diag.result;
        int64_t g = gcd_i64(a,b); if (id == N_GCD) { *out = make_int(g); return VANTA_OK; }
        if (g == 0) { *out = make_int(0); return VANTA_OK; }
        *out = make_int((int64_t)((uint64_t)(a / g) * (uint64_t)b)); return VANTA_OK;
    }
    if (id == N_SQRT) return native_numeric_unary(vm, argv[0], m_sqrt, out, "sqrt");
    if (id == N_CBRT) return native_numeric_unary(vm, argv[0], m_cbrt, out, "cbrt");
    if (id == N_EXP) return native_numeric_unary(vm, argv[0], m_exp, out, "exp");
    if (id == N_LOG) return native_numeric_unary(vm, argv[0], m_log, out, "log");
    if (id == N_LOG10) return native_numeric_unary(vm, argv[0], m_log10, out, "log10");
    if (id == N_FLOOR) return native_numeric_unary(vm, argv[0], m_floor, out, "floor");
    if (id == N_CEIL) return native_numeric_unary(vm, argv[0], m_ceil, out, "ceil");
    if (id == N_ROUND) return native_numeric_unary(vm, argv[0], m_round, out, "round");
    if (id == N_TRUNC) return native_numeric_unary(vm, argv[0], m_trunc, out, "trunc");
    if (id == N_SIN) return native_numeric_unary(vm, argv[0], m_sin, out, "sin");
    if (id == N_COS) return native_numeric_unary(vm, argv[0], m_cos, out, "cos");
    if (id == N_TAN) return native_numeric_unary(vm, argv[0], m_tan, out, "tan");
    if (id == N_ASIN) return native_numeric_unary(vm, argv[0], m_asin, out, "asin");
    if (id == N_ACOS) return native_numeric_unary(vm, argv[0], m_acos, out, "acos");
    if (id == N_ATAN) return native_numeric_unary(vm, argv[0], m_atan, out, "atan");
    if (id == N_SINH) return native_numeric_unary(vm, argv[0], m_sinh, out, "sinh");
    if (id == N_COSH) return native_numeric_unary(vm, argv[0], m_cosh, out, "cosh");
    if (id == N_TANH) return native_numeric_unary(vm, argv[0], m_tanh, out, "tanh");
    if (id == N_POW || id == N_FMOD || id == N_HYPOT || id == N_ATAN2) {
        double a,b; if (!value_as_number(vm, argv[0], &a, native_name(id)) || !value_as_number(vm, argv[1], &b, native_name(id))) return vm->diag.result;
        if (id == N_POW) *out = make_float(m_pow(a,b));
        else if (id == N_FMOD) *out = make_float(m_fmod(a,b));
        else if (id == N_HYPOT) *out = make_float(m_hypot(a,b));
        else *out = make_float(m_atan2(a,b));
        return VANTA_OK;
    }
    if (id >= N_ISNAN && id <= N_ISFN) {
        VantaValue a = argv[0]; bool hit = false;
        switch (id) {
            case N_ISNAN: hit = a.type == V_FLOAT && m_isnan(a.as.f); break;
            case N_ISINF: hit = a.type == V_FLOAT && m_isinf(a.as.f); break;
            case N_ISINT: hit = a.type == V_INT; break; case N_ISFLOAT: hit = a.type == V_FLOAT; break;
            case N_ISSTR: hit = a.type == V_STR; break; case N_ISARR: hit = a.type == V_ARR; break;
            case N_ISMAP: hit = a.type == V_MAP; break; case N_ISFN: hit = a.type == V_FUNC || a.type == V_NATIVE; break;
            default: break;
        }
        *out = make_bool(hit); return VANTA_OK;
    }
    if (id == N_RAND) { *out = make_float(rng_next_f64(vm)); return VANTA_OK; }
    if (id == N_RANDSEED) { int64_t seed; if (!value_as_int(vm, argv[0], &seed, "randSeed")) return vm->diag.result; rng_seed(vm, (uint64_t)seed); return VANTA_OK; }
    if (id == N_RANDINT) { int64_t lo,hi; if (!value_as_int(vm, argv[0], &lo, "randInt") || !value_as_int(vm, argv[1], &hi, "randInt")) return vm->diag.result; if (lo > hi) return rt_error(vm, VANTA_ERR_RUNTIME, "randInt lower bound exceeds upper bound"); *out = make_int(rng_range_i64(vm,lo,hi)); return VANTA_OK; }
    if (id == N_RANDCHOICE) { if (argv[0].type != V_ARR || arr_len(vm, argv[0].as.h) == 0) return rt_error(vm, VANTA_ERR_TYPE, "randChoice requires a non-empty array"); uint32_t pick = (uint32_t)rng_bounded_u64(vm, arr_len(vm, argv[0].as.h)); arr_get(vm, argv[0].as.h, pick, out); return VANTA_OK; }
    if (id == N_ARRAY) { uint32_t h = arr_new(vm, argc); if (h == VANTA_NIL_HANDLE) return native_oom(vm); for (uint32_t i=0;i<argc;i++) if(!arr_push(vm,h,argv[i])) { arr_release(vm,h); return native_oom(vm); } *out=make_arr(h); return VANTA_OK; }
    if (id == N_ARRAYOF) { int64_t n; if(!value_as_int(vm,argv[0],&n,"arrayOf")) return vm->diag.result; if(n<0 || (uint64_t)n>VANTA_ARR_MAX) return rt_error(vm,VANTA_ERR_INDEX_BOUNDS,"arrayOf size out of range"); uint32_t h=arr_new(vm,(uint32_t)n); if(h==VANTA_NIL_HANDLE) return native_oom(vm); for(uint32_t i=0;i<(uint32_t)n;i++) if(!arr_push(vm,h,argv[1])) {arr_release(vm,h);return native_oom(vm);} *out=make_arr(h);return VANTA_OK; }
    if (id == N_MAP) { uint32_t h=map_new(vm,8); if(h==VANTA_NIL_HANDLE)return native_oom(vm); *out=make_map(h);return VANTA_OK; }
    if (id == N_RANGE) {
        int64_t begin=0,end=0,step=1;
        if(!value_as_int(vm,argv[0],&end,"range")) return vm->diag.result;
        if(argc>=2) {begin=end; if(!value_as_int(vm,argv[1],&end,"range")) return vm->diag.result;}
        if(argc==3 && !value_as_int(vm,argv[2],&step,"range")) return vm->diag.result;
        if(step==0) return rt_error(vm,VANTA_ERR_RUNTIME,"range step cannot be zero");
        uint32_t h=arr_new(vm,16); if(h==VANTA_NIL_HANDLE)return native_oom(vm);
        for(int64_t i=begin;(step>0?i<end:i>end);i=(int64_t)((uint64_t)i+(uint64_t)step)) {if(arr_len(vm,h)>=VANTA_ARR_MAX||!arr_push(vm,h,make_int(i))){arr_release(vm,h);return native_oom(vm);}}
        *out=make_arr(h);return VANTA_OK;
    }
    if (id == N_SORT || id == N_REVERSE) {
        if(argv[0].type!=V_ARR) return rt_error(vm,VANTA_ERR_TYPE,"%s requires an array",native_name(id));
        uint32_t h=arr_new(vm,arr_len(vm,argv[0].as.h));if(h==VANTA_NIL_HANDLE)return native_oom(vm);
        for(uint32_t i=0;i<arr_len(vm,argv[0].as.h);i++){VantaValue v;arr_get(vm,argv[0].as.h,i,&v);if(!arr_push(vm,h,v)){value_release(vm,v);arr_release(vm,h);return native_oom(vm);}value_release(vm,v);}
        if(id==N_SORT)arr_sort(vm,h);else arr_reverse(vm,h);*out=make_arr(h);return VANTA_OK;
    }
    if (id == N_HASH) { sb_reset(&vm->sb); value_to_sb(vm,&vm->sb,argv[0]); *out=make_int((int64_t)(uint64_t)v_hash_bytes((const uint8_t*)vm->sb.space,vm->sb.len)); return VANTA_OK; }
    if (id == N_CHR) { int64_t cp; char b[4]; if(!value_as_int(vm,argv[0],&cp,"chr")||cp<0||(uint64_t)cp>0x10FFFFu)return rt_error(vm,VANTA_ERR_TYPE,"chr requires a Unicode code point"); uint32_t n=utf8_encode((uint32_t)cp,b);return make_string_bytes(vm,b,n,out); }
    if (id == N_ORD) { if(argv[0].type!=V_STR||utf8_length(str_data(vm,argv[0].as.h),str_len(vm,argv[0].as.h))!=1)return rt_error(vm,VANTA_ERR_TYPE,"ord requires a one-character string");uint32_t cp;utf8_decode(str_data(vm,argv[0].as.h),str_len(vm,argv[0].as.h),&cp);*out=make_int((int64_t)cp);return VANTA_OK; }
    if (id == N_SYSCALL) { int64_t sid,a=0,b=0;if(!value_as_int(vm,argv[0],&sid,"syscall"))return vm->diag.result;if(argc>1&&!value_as_int(vm,argv[1],&a,"syscall"))return vm->diag.result;if(argc>2&&!value_as_int(vm,argv[2],&b,"syscall"))return vm->diag.result;int rc=vanta_syscall_hook((int)sid,a,b);if(rc==-1)return rt_error(vm,VANTA_ERR_UNKNOWN_SYSCALL,"unknown syscall %lld",(long long)sid);*out=make_int(rc);return VANTA_OK; }
    if (id == N_ASSERT) { if(!value_truthy(vm,argv[0])) { if(argc==2&&argv[1].type==V_STR)return rt_error(vm,VANTA_ERR_RUNTIME,"assertion failed: %s",str_data(vm,argv[1].as.h));return rt_error(vm,VANTA_ERR_RUNTIME,"assertion failed");}return VANTA_OK; }
    if (id == N_EXIT) { vm->exit_code=argc?(argv[0].type==V_INT?argv[0].as.i:0):0; if(argc&&argv[0].type!=V_INT)return rt_error(vm,VANTA_ERR_TYPE,"exit code must be an integer");return VANTA_ERR_HALT; }
    if (id == N_CLOCK) { *out=make_int((int64_t)vanta_syscall_hook(2,0,0));return VANTA_OK; }
    return rt_error(vm,VANTA_ERR_RUNTIME,"native '%s' dispatch missing",native_name(id));
}

/* P18 - checked bytecode dispatch and public API */

static VantaResult runtime_local_index(VantaVM *vm, uint32_t slot, uint32_t *out) {
    if (vm->frame_count == 0) return rt_error(vm, VANTA_ERR_INTERNAL, "local access without frame");
    uint32_t base = vm->frames[vm->frame_count - 1].locals_base;
    if (slot > VANTA_MAX_LOCALS - base - 1) return rt_error(vm, VANTA_ERR_INTERNAL, "local slot out of range");
    *out = base + slot;
    return VANTA_OK;
}

static VantaResult runtime_index_get(VantaVM *vm, VantaValue obj, VantaValue key,
                                     VantaValue *out) {
    *out = VANTA_VALUE_NIL;
    if (obj.type == V_ARR) {
        int64_t idx;
        if (!value_as_int(vm, key, &idx, "array index")) return vm->diag.result;
        if (idx < 0 || !arr_get(vm, obj.as.h, (uint32_t)idx, out)) {
            return rt_error(vm, VANTA_ERR_INDEX_BOUNDS, "array index %lld out of range", (long long)idx);
        }
        return VANTA_OK;
    }
    if (obj.type == V_STR) {
        int64_t idx;
        if (!value_as_int(vm, key, &idx, "string index")) return vm->diag.result;
        uint32_t cps = utf8_length(str_data(vm, obj.as.h), str_len(vm, obj.as.h));
        if (idx < 0 || (uint64_t)idx >= cps) {
            return rt_error(vm, VANTA_ERR_INDEX_BOUNDS, "string index %lld out of range", (long long)idx);
        }
        uint32_t off = utf8_byte_offset(str_data(vm, obj.as.h), str_len(vm, obj.as.h), (uint32_t)idx);
        uint32_t cp;
        uint32_t bytes = utf8_decode(str_data(vm, obj.as.h) + off, str_len(vm, obj.as.h) - off, &cp);
        return make_string_bytes(vm, str_data(vm, obj.as.h) + off, bytes, out);
    }
    if (obj.type == V_MAP) {
        bool found;
        if (key.type != V_STR) return rt_error(vm, VANTA_ERR_TYPE, "map key must be a string, got %s", value_type_name(key));
        if (!map_get(vm, obj.as.h, key.as.h, out, &found)) return rt_error(vm, VANTA_ERR_RUNTIME, "invalid map handle");
        return VANTA_OK;
    }
    return rt_error(vm, VANTA_ERR_TYPE, "cannot index %s", value_type_name(obj));
}

static VantaResult runtime_index_set(VantaVM *vm, VantaValue obj, VantaValue key,
                                     VantaValue val) {
    if (obj.type == V_ARR) {
        int64_t idx;
        if (!value_as_int(vm, key, &idx, "array index")) return vm->diag.result;
        if (idx < 0 || !arr_set(vm, obj.as.h, (uint32_t)idx, val)) {
            return rt_error(vm, VANTA_ERR_INDEX_BOUNDS, "array index %lld out of range", (long long)idx);
        }
        return VANTA_OK;
    }
    if (obj.type == V_MAP) {
        if (key.type != V_STR) return rt_error(vm, VANTA_ERR_TYPE, "map key must be a string, got %s", value_type_name(key));
        if (!map_set(vm, obj.as.h, key.as.h, val)) return native_oom(vm);
        return VANTA_OK;
    }
    if (obj.type == V_STR) return rt_error(vm, VANTA_ERR_TYPE, "strings are immutable");
    return rt_error(vm, VANTA_ERR_TYPE, "cannot assign through %s", value_type_name(obj));
}

static void runtime_cleanup(VantaVM *vm) {
    while (vm->sp) value_release(vm, vm->stack[--vm->sp]);
    for (uint32_t i = 0; i < vm->locals_top; i++) {
        value_release(vm, vm->locals[i]);
        vm->locals[i] = VANTA_VALUE_NIL;
    }
    vm->locals_top = 0;
    vm->frame_count = 0;
    vm->fp = VANTA_NO_INDEX;
}

static VantaResult runtime_fail_cleanup(VantaVM *vm, VantaResult result) {
    runtime_cleanup(vm);
    vm->running = 0;
    return result;
}

static VantaResult interpret(VantaVM *vm) {
    vm->pc = 0;
    vm->sp = 0;
    vm->locals_top = 0;
    vm->frame_count = 0;
    vm->fp = VANTA_NO_INDEX;
    vm->exit_code = 0;
    if (vm->fuel == 0) vm->fuel = VANTA_DEFAULT_FUEL;
    if (vm->local_count > VANTA_MAX_LOCALS) return rt_error(vm, VANTA_ERR_INTERNAL, "main local layout invalid");
    if (frame_push(vm, VANTA_NO_INDEX, VANTA_NO_INDEX, 0, 0) != VANTA_OK) return vm->diag.result;
    vm->locals_top = vm->local_count;
    for (uint32_t i = 0; i < vm->locals_top; i++) vm->locals[i] = VANTA_VALUE_NIL;
    vm->fp = 0;
    vm->running = 1;
    uint32_t last_trace_line = VANTA_NO_INDEX;

    while (vm->pc < vm->bc_len) {
        if (vm->fuel == 0) return runtime_fail_cleanup(vm, rt_error(vm, VANTA_ERR_FUEL_EXHAUSTED, "instruction fuel exhausted"));
        vm->fuel--;
        uint32_t op_pc = vm->pc;
        uint32_t op = vm->bytecode[vm->pc++];
        if (op >= OP_COUNT) return runtime_fail_cleanup(vm, rt_error(vm, VANTA_ERR_INTERNAL, "invalid opcode %u at pc=%u", op, op_pc));

        if (vm->trace_on) {
            uint32_t line = pc_to_line(vm, op_pc);
            if (line && line != last_trace_line) {
                char trace_line[48];
                vant_snprintf(trace_line, sizeof(trace_line), "[vanta:%u]\n", line);
                vanta_out(vm, trace_line);
                last_trace_line = line;
            }
        }

        VantaResult r = VANTA_OK;
        uint32_t arg = 0;
        switch (op) {
            case OP_NOP: break;
            case OP_HALT:
                runtime_cleanup(vm);
                vm->running = 0;
                return VANTA_OK;
            case OP_PUSH_NIL: r = push_value(vm, VANTA_VALUE_NIL); break;
            case OP_PUSH_TRUE: r = push_value(vm, VANTA_VALUE_TRUE); break;
            case OP_PUSH_FALSE: r = push_value(vm, VANTA_VALUE_FALSE); break;
            case OP_PUSH_INT: {
                int64_t v; r = read_i64(vm, vm->pc, &v); if (r) break; vm->pc += 8; r = push_value(vm, make_int(v)); break;
            }
            case OP_PUSH_FLOAT: {
                double v; r = read_f64(vm, vm->pc, &v); if (r) break; vm->pc += 8; r = push_value(vm, make_float(v)); break;
            }
            case OP_PUSH_CONST: {
                r = read_u32(vm, vm->pc, &arg); if (r) break; vm->pc += 4;
                if (arg >= vm->const_count) { r = rt_error(vm, VANTA_ERR_INTERNAL, "constant index %u out of range", arg); break; }
                uint32_t h = vm->const_str_handles[arg];
                if (h == VANTA_NIL_HANDLE) {
                    h = str_alloc(vm, vm->const_data + vm->consts[arg].off, vm->consts[arg].len);
                    if (h == VANTA_NIL_HANDLE) { r = native_oom(vm); break; }
                    vm->const_str_handles[arg] = h;
                }
                str_retain(vm, h);
                r = push_value(vm, make_str(h));
                if (r) str_release(vm, h);
                break;
            }
            case OP_LOAD_LOCAL: {
                uint32_t idx; r = read_u32(vm, vm->pc, &arg); if (r) break; vm->pc += 4;
                r = runtime_local_index(vm, arg, &idx); if (r) break;
                VantaValue v = vm->locals[idx]; value_retain(vm, v); r = push_value(vm, v); if (r) value_release(vm, v); break;
            }
            case OP_STORE_LOCAL: {
                uint32_t idx; r = read_u32(vm, vm->pc, &arg); if (r) break; vm->pc += 4;
                if (vm->sp == 0) { r = rt_error(vm, VANTA_ERR_INTERNAL, "STORE_LOCAL stack underflow"); break; }
                r = runtime_local_index(vm, arg, &idx); if (r) break;
                VantaValue v = pop_value(vm); value_release(vm, vm->locals[idx]); vm->locals[idx] = v; break;
            }
            case OP_LOAD_GLOBAL: {
                r = read_u32(vm, vm->pc, &arg); if (r) break; vm->pc += 4;
                if (arg >= vm->global_count) { r = rt_error(vm, VANTA_ERR_INTERNAL, "global slot %u out of range", arg); break; }
                VantaValue v = vm->globals[arg].value; value_retain(vm, v); r = push_value(vm, v); if (r) value_release(vm, v); break;
            }
            case OP_STORE_GLOBAL: {
                r = read_u32(vm, vm->pc, &arg); if (r) break; vm->pc += 4;
                if (arg >= vm->global_count || vm->sp == 0) { r = rt_error(vm, VANTA_ERR_INTERNAL, "STORE_GLOBAL invariant broken"); break; }
                if (vm->globals[arg].flags & GLOBAL_FLAG_CONST) { r = rt_error(vm, VANTA_ERR_REASSIGN_CONST, "cannot assign to const '%s'", global_name_str(vm, arg)); break; }
                VantaValue v = pop_value(vm); value_release(vm, vm->globals[arg].value); vm->globals[arg].value = v; vm->globals[arg].flags |= GLOBAL_FLAG_DEFINED; break;
            }
            case OP_POP: r = pop_discard(vm); break;
            case OP_DUP:
                if (vm->sp == 0) r = rt_error(vm, VANTA_ERR_INTERNAL, "DUP stack underflow");
                else { VantaValue v = vm->stack[vm->sp - 1]; value_retain(vm, v); r = push_value(vm, v); if (r) value_release(vm, v); }
                break;
            case OP_DUP2:
                if (vm->sp < 2 || vm->sp > VANTA_MAX_STACK - 2) r = rt_error(vm, VANTA_ERR_STACK_OVERFLOW, "DUP2 stack invariant broken");
                else { VantaValue a = vm->stack[vm->sp - 2], b = vm->stack[vm->sp - 1]; value_retain(vm, a); value_retain(vm, b); vm->stack[vm->sp++] = a; vm->stack[vm->sp++] = b; }
                break;
            case OP_SWAP:
                if (vm->sp < 2) r = rt_error(vm, VANTA_ERR_INTERNAL, "SWAP stack underflow");
                else { VantaValue t = vm->stack[vm->sp - 1]; vm->stack[vm->sp - 1] = vm->stack[vm->sp - 2]; vm->stack[vm->sp - 2] = t; }
                break;
            case OP_ADD:
            case OP_SUB:
            case OP_MUL:
            case OP_DIV:
            case OP_MOD: {
                if (vm->sp < 2) { r = rt_error(vm, VANTA_ERR_INTERNAL, "binary operator stack underflow"); break; }
                VantaValue b = pop_value(vm), a = pop_value(vm), result = VANTA_VALUE_NIL;
                if (op == OP_ADD && (a.type == V_STR || b.type == V_STR)) {
                    sb_reset(&vm->sb); value_to_sb(vm, &vm->sb, a); value_to_sb(vm, &vm->sb, b);
                    uint32_t h = str_take_sb(vm, &vm->sb);
                    if (h == VANTA_NIL_HANDLE) r = native_oom(vm); else result = make_str(h);
                } else if (op == OP_ADD && a.type == V_ARR && b.type == V_ARR) {
                    uint32_t h = arr_concat(vm, a.as.h, b.as.h);
                    if (h == VANTA_NIL_HANDLE) r = native_oom(vm); else result = make_arr(h);
                } else if ((a.type == V_INT || a.type == V_FLOAT) && (b.type == V_INT || b.type == V_FLOAT)) {
                    if (a.type == V_INT && b.type == V_INT && op != OP_DIV) {
                        if (op == OP_ADD) result = make_int((int64_t)((uint64_t)a.as.i + (uint64_t)b.as.i));
                        else if (op == OP_SUB) result = make_int((int64_t)((uint64_t)a.as.i - (uint64_t)b.as.i));
                        else if (op == OP_MUL) result = make_int((int64_t)((uint64_t)a.as.i * (uint64_t)b.as.i));
                        else {
                            if (b.as.i == 0) r = rt_error(vm, VANTA_ERR_DIV_BY_ZERO, "integer modulo by zero");
                            else result = make_int(a.as.i % b.as.i);
                        }
                    } else {
                        double x = a.type == V_INT ? (double)a.as.i : a.as.f;
                        double y = b.type == V_INT ? (double)b.as.i : b.as.f;
                        if (op == OP_ADD) result = make_float(x + y);
                        else if (op == OP_SUB) result = make_float(x - y);
                        else if (op == OP_MUL) result = make_float(x * y);
                        else if (op == OP_DIV) { if (y == 0.0) r = rt_error(vm, VANTA_ERR_DIV_BY_ZERO, "division by zero"); else result = make_float(x / y); }
                        else { if (y == 0.0) r = rt_error(vm, VANTA_ERR_DIV_BY_ZERO, "modulo by zero"); else result = make_float(m_fmod(x, y)); }
                    }
                    if (op == OP_DIV && a.type == V_INT && b.type == V_INT) {
                        if (b.as.i == 0) r = rt_error(vm, VANTA_ERR_DIV_BY_ZERO, "integer division by zero");
                        else if (a.as.i == (-9223372036854775807LL - 1LL) && b.as.i == -1) result = make_int(a.as.i);
                        else result = make_int(a.as.i / b.as.i);
                    }
                } else r = rt_error(vm, VANTA_ERR_TYPE, "%s requires numbers or supported matching types, got %s and %s", op_info[op].name, value_type_name(a), value_type_name(b));
                value_release(vm, a); value_release(vm, b);
                if (r == VANTA_OK) { r = push_value(vm, result); if (r) value_release(vm, result); }
                break;
            }
            case OP_NEG:
            case OP_BNOT:
            case OP_NOT: {
                if (vm->sp == 0) { r = rt_error(vm, VANTA_ERR_INTERNAL, "unary operator stack underflow"); break; }
                VantaValue a = pop_value(vm), result = VANTA_VALUE_NIL;
                if (op == OP_NOT) result = make_bool(!value_truthy(vm, a));
                else if (op == OP_NEG && a.type == V_INT) result = make_int((int64_t)(0 - (uint64_t)a.as.i));
                else if (op == OP_NEG && a.type == V_FLOAT) result = make_float(-a.as.f);
                else if (op == OP_BNOT && a.type == V_INT) result = make_int(~a.as.i);
                else r = rt_error(vm, VANTA_ERR_TYPE, "%s requires %s", op_info[op].name, op == OP_BNOT ? "an integer" : "a number");
                value_release(vm, a); if (!r) { r = push_value(vm, result); if (r) value_release(vm, result); } break;
            }
            case OP_BAND: case OP_BOR: case OP_BXOR: case OP_SHL: case OP_SHR: {
                if (vm->sp < 2) { r = rt_error(vm, VANTA_ERR_INTERNAL, "bitwise operator stack underflow"); break; }
                VantaValue b = pop_value(vm), a = pop_value(vm);
                if (a.type != V_INT || b.type != V_INT) r = rt_error(vm, VANTA_ERR_TYPE, "%s requires integers", op_info[op].name);
                else if ((op == OP_SHL || op == OP_SHR) && (b.as.i < 0 || b.as.i > 63)) r = rt_error(vm, VANTA_ERR_INDEX_BOUNDS, "shift count must be 0..63");
                else {
                    int64_t v;
                    if (op == OP_BAND) v = a.as.i & b.as.i;
                    else if (op == OP_BOR) v = a.as.i | b.as.i;
                    else if (op == OP_BXOR) v = a.as.i ^ b.as.i;
                    else if (op == OP_SHL) v = (int64_t)((uint64_t)a.as.i << (uint32_t)b.as.i);
                    else if (b.as.i == 0) v = a.as.i;
                    else if (a.as.i >= 0) v = a.as.i >> (uint32_t)b.as.i;
                    else v = (int64_t)(((uint64_t)a.as.i >> (uint32_t)b.as.i) | (~(uint64_t)0 << (64u - (uint32_t)b.as.i)));
                    r = push_value(vm, make_int(v));
                }
                value_release(vm, a); value_release(vm, b); break;
            }
            case OP_EQ: case OP_NE: case OP_LT: case OP_LE: case OP_GT: case OP_GE: {
                if (vm->sp < 2) { r = rt_error(vm, VANTA_ERR_INTERNAL, "comparison stack underflow"); break; }
                VantaValue b = pop_value(vm), a = pop_value(vm); bool answer;
                if (op == OP_EQ || op == OP_NE) answer = value_equal(vm, a, b);
                else { bool ok; int cmp = value_compare(vm, a, b, &ok); if (!ok) r = rt_error(vm, VANTA_ERR_TYPE, "cannot compare %s and %s", value_type_name(a), value_type_name(b)); answer = op == OP_LT ? cmp < 0 : op == OP_LE ? cmp <= 0 : op == OP_GT ? cmp > 0 : cmp >= 0; }
                value_release(vm, a); value_release(vm, b);
                if (!r) { if (op == OP_NE) answer = !answer; r = push_value(vm, make_bool(answer)); } break;
            }
            case OP_INC_LOCAL:
            case OP_DEC_LOCAL: {
                uint32_t idx; r = read_u32(vm, vm->pc, &arg); if (r) break; vm->pc += 4; r = runtime_local_index(vm, arg, &idx); if (r) break;
                VantaValue old = vm->locals[idx], fresh;
                if (old.type == V_INT) fresh = make_int((int64_t)((uint64_t)old.as.i + (op == OP_INC_LOCAL ? 1u : ~(uint64_t)0)));
                else if (old.type == V_FLOAT) fresh = make_float(old.as.f + (op == OP_INC_LOCAL ? 1.0 : -1.0));
                else { r = rt_error(vm, VANTA_ERR_TYPE, "%s requires a numeric variable", op_info[op].name); break; }
                value_release(vm, old); vm->locals[idx] = fresh; break;
            }
            case OP_JMP:
                r = read_u32(vm, vm->pc, &arg); if (r) break; if (arg > vm->bc_len) { r = rt_error(vm,VANTA_ERR_INTERNAL,"jump target out of range"); break; } vm->pc = arg; break;
            case OP_JMP_FALSE:
            case OP_JMP_FALSE_POP:
            case OP_JMP_TRUE_POP: {
                r = read_u32(vm, vm->pc, &arg); if (r) break; vm->pc += 4;
                if (arg > vm->bc_len || vm->sp == 0) { r = rt_error(vm,VANTA_ERR_INTERNAL,"conditional jump invariant broken"); break; }
                VantaValue cond = vm->stack[vm->sp - 1]; bool truth = value_truthy(vm, cond);
                if (op != OP_JMP_FALSE) { cond = pop_value(vm); value_release(vm, cond); }
                if ((op == OP_JMP_FALSE || op == OP_JMP_FALSE_POP) ? !truth : truth) vm->pc = arg;
                break;
            }
            case OP_INDEX: {
                if (vm->sp < 2) { r = rt_error(vm,VANTA_ERR_INTERNAL,"INDEX stack underflow"); break; }
                VantaValue key=pop_value(vm), obj=pop_value(vm), result;
                r=runtime_index_get(vm,obj,key,&result); value_release(vm,obj); value_release(vm,key);
                if(!r){r=push_value(vm,result);if(r)value_release(vm,result);} break;
            }
            case OP_INDEX_SET:
            case OP_INDEX_SET_KEEP: {
                uint32_t need = op == OP_INDEX_SET ? 3u : 3u;
                if (vm->sp < need) { r=rt_error(vm,VANTA_ERR_INTERNAL,"INDEX_SET stack underflow"); break; }
                VantaValue val = vm->stack[vm->sp - 1], key = vm->stack[vm->sp - 2], obj = vm->stack[vm->sp - 3];
                r = runtime_index_set(vm,obj,key,val);
                if (op == OP_INDEX_SET) { vm->sp -= 3; value_release(vm,obj); value_release(vm,key); value_release(vm,val); }
                else { vm->stack[vm->sp - 3] = val; vm->sp -= 2; value_release(vm,obj); value_release(vm,key); }
                break;
            }
            case OP_CALL:
                r = read_u32(vm, vm->pc, &arg); if (r) break; vm->pc += 4; if (arg > VANTA_MAX_ARGS) r = rt_error(vm,VANTA_ERR_ARITY,"too many call arguments"); else r = do_call(vm,arg); break;
            case OP_CALL_METHOD: {
                r=read_u32(vm,vm->pc,&arg);if(r)break;vm->pc+=4;uint32_t argc=arg&0xFFFFu, method=arg>>16;
                if(argc>VANTA_MAX_ARGS||vm->sp<argc+1){r=rt_error(vm,VANTA_ERR_INTERNAL,"method call stack invariant broken");break;}
                VantaValue recv=vm->stack[vm->sp-argc-1],*args=&vm->stack[vm->sp-argc],result=VANTA_VALUE_NIL;
                r=call_method(vm,method,recv,argc,args,&result);
                for(uint32_t i=0;i<argc;i++)value_release(vm,args[i]);value_release(vm,recv);vm->sp-=argc+1;
                if(!r){r=push_value(vm,result);if(r)value_release(vm,result);}break;
            }
            case OP_RETURN:
                if(vm->sp==0){r=rt_error(vm,VANTA_ERR_INTERNAL,"RETURN stack underflow");break;}
                r=do_return(vm,pop_value(vm));break;
            case OP_RETURN_NIL:
                r=do_return(vm,VANTA_VALUE_NIL);break;
            case OP_MAKE_FN:
                r=read_u32(vm,vm->pc,&arg);if(r)break;vm->pc+=4;
                if(arg&0x80000000u){uint32_t id=arg&0x7FFFFFFFu;if(id>=N_NATIVE_COUNT)r=rt_error(vm,VANTA_ERR_INTERNAL,"native id out of range");else r=push_value(vm,make_native(id));}
                else if(arg>=vm->function_count)r=rt_error(vm,VANTA_ERR_INTERNAL,"function id out of range");else r=push_value(vm,make_func(arg));
                break;
            case OP_ITER_BEGIN: {
                if(vm->sp==0){r=rt_error(vm,VANTA_ERR_INTERNAL,"ITER_BEGIN stack underflow");break;}
                VantaValue obj=pop_value(vm);
                if(obj.type==V_MAP){uint32_t h=map_keys_array(vm,obj.as.h);value_release(vm,obj);if(h==VANTA_NIL_HANDLE){r=native_oom(vm);break;}obj=make_arr(h);}
                if(obj.type!=V_ARR&&obj.type!=V_STR){value_release(vm,obj);r=rt_error(vm,VANTA_ERR_TYPE,"cannot iterate %s",value_type_name(obj));break;}
                r=push_value(vm,obj);if(!r)r=push_value(vm,make_int(0));if(r&&vm->sp) {VantaValue dead=pop_value(vm);value_release(vm,dead);}break;
            }
            case OP_ITER_NEXT: {
                r=read_u32(vm,vm->pc,&arg);if(r)break;vm->pc+=4;
                if(vm->sp<2||arg>vm->bc_len){r=rt_error(vm,VANTA_ERR_INTERNAL,"ITER_NEXT invariant broken");break;}
                VantaValue *obj=&vm->stack[vm->sp-2],*idx=&vm->stack[vm->sp-1];
                if(idx->type!=V_INT){r=rt_error(vm,VANTA_ERR_INTERNAL,"iterator index corrupt");break;}
                if(obj->type==V_ARR){uint32_t n=arr_len(vm,obj->as.h);if(idx->as.i<0||(uint64_t)idx->as.i>=n)vm->pc=arg;else{VantaValue e;if(!arr_get(vm,obj->as.h,(uint32_t)idx->as.i,&e)){r=rt_error(vm,VANTA_ERR_INTERNAL,"iterator array access failed");break;}idx->as.i++;r=push_value(vm,e);if(r)value_release(vm,e);}}
                else if(obj->type==V_STR){uint32_t n=str_len(vm,obj->as.h);if(idx->as.i<0||(uint64_t)idx->as.i>=n)vm->pc=arg;else{uint32_t cp,bytes=utf8_decode(str_data(vm,obj->as.h)+(uint32_t)idx->as.i,n-(uint32_t)idx->as.i,&cp);VantaValue e;r=make_string_bytes(vm,str_data(vm,obj->as.h)+(uint32_t)idx->as.i,bytes,&e);if(r)break;idx->as.i+=bytes;r=push_value(vm,e);if(r)value_release(vm,e);}}
                else r=rt_error(vm,VANTA_ERR_INTERNAL,"iterator object corrupt");
                break;
            }
            case OP_ITER_END:
                if(vm->sp<2)r=rt_error(vm,VANTA_ERR_INTERNAL,"ITER_END stack underflow");else{value_release(vm,pop_value(vm));value_release(vm,pop_value(vm));}break;
            case OP_SYSCALL: {
                r=read_u32(vm,vm->pc,&arg);if(r)break;vm->pc+=4;
                if(arg<1||arg>3||vm->sp<arg){r=rt_error(vm,VANTA_ERR_INTERNAL,"SYSCALL stack invariant broken");break;}
                VantaValue vals[3];for(uint32_t i=0;i<arg;i++)vals[arg-1-i]=pop_value(vm);
                int64_t sid,a=0,b=0;bool ok=value_as_int(vm,vals[0],&sid,"syscall");
                if(ok&&arg>1)ok=value_as_int(vm,vals[1],&a,"syscall");if(ok&&arg>2)ok=value_as_int(vm,vals[2],&b,"syscall");
                for(uint32_t i=0;i<arg;i++)value_release(vm,vals[i]);if(!ok){r=vm->diag.result;break;}
                int rc=vanta_syscall_hook((int)sid,a,b);if(rc==-1)r=rt_error(vm,VANTA_ERR_UNKNOWN_SYSCALL,"unknown syscall %lld",(long long)sid);else r=push_value(vm,make_int(rc));break;
            }
            case OP_ARRAY_NEW: {
                r=read_u32(vm,vm->pc,&arg);if(r)break;vm->pc+=4;if(arg>VANTA_ARR_MAX||vm->sp<arg){r=rt_error(vm,VANTA_ERR_INTERNAL,"ARRAY_NEW stack invariant broken");break;}
                uint32_t h=arr_new(vm,arg);if(h==VANTA_NIL_HANDLE){r=native_oom(vm);break;}
                for(uint32_t i=0;i<arg;i++)if(!arr_push(vm,h,vm->stack[vm->sp-arg+i])){r=native_oom(vm);break;}
                if(r){arr_release(vm,h);break;}for(uint32_t i=0;i<arg;i++)value_release(vm,vm->stack[vm->sp-arg+i]);vm->sp-=arg;r=push_value(vm,make_arr(h));if(r)arr_release(vm,h);break;
            }
            case OP_MAP_NEW: {
                r=read_u32(vm,vm->pc,&arg);if(r)break;vm->pc+=4;if(arg>VANTA_MAP_BUCKETS_MAX||vm->sp<arg*2u){r=rt_error(vm,VANTA_ERR_INTERNAL,"MAP_NEW stack invariant broken");break;}
                uint32_t h=map_new(vm,arg*2u);if(h==VANTA_NIL_HANDLE){r=native_oom(vm);break;}
                for(uint32_t i=0;i<arg;i++){VantaValue key=vm->stack[vm->sp-arg*2u+i*2u],val=vm->stack[vm->sp-arg*2u+i*2u+1u];if(key.type!=V_STR){r=rt_error(vm,VANTA_ERR_TYPE,"map key must be a string");break;}if(!map_set(vm,h,key.as.h,val)){r=native_oom(vm);break;}}
                if(r){map_release(vm,h);break;}for(uint32_t i=0;i<arg*2u;i++)value_release(vm,vm->stack[vm->sp-arg*2u+i]);vm->sp-=arg*2u;r=push_value(vm,make_map(h));if(r)map_release(vm,h);break;
            }
            default: r=rt_error(vm,VANTA_ERR_INTERNAL,"unhandled opcode %u",op);break;
        }
        if(r!=VANTA_OK) return runtime_fail_cleanup(vm,r);
    }
    runtime_cleanup(vm); vm->running=0;
    return VANTA_OK;
}

VantaResult vanta_init(VantaVM *vm, const char *source, size_t len) {
    if (!vm || !source) return VANTA_ERR_RUNTIME;
    if (len > VANTA_MAX_SRC) return VANTA_ERR_SOURCE_TOO_LARGE;
    v_memset(vm, 0, sizeof(*vm));
    vm->source = source;
    vm->src_len = (uint32_t)len;
    vm->cur_fn = VANTA_NO_INDEX;
    vm->fp = VANTA_NO_INDEX;
    vm->fuel = VANTA_DEFAULT_FUEL;
    vm->diag.result = VANTA_OK;
    for (uint32_t i = 0; i < VANTA_MAX_CONSTS; i++) vm->const_str_handles[i] = VANTA_NIL_HANDLE;
    if (!str_pool_init(vm) || !obj_pool_init(vm)) {
        vm->diag.result = VANTA_ERR_OUT_OF_MEMORY;
        return VANTA_ERR_OUT_OF_MEMORY;
    }
    rng_seed(vm, RNG_DEFAULT_SEED);
    return VANTA_OK;
}

VantaResult vanta_compile(VantaVM *vm) {
    if (!vm || !vm->source) return VANTA_ERR_RUNTIME;
    vm->diag.result = VANTA_OK;
    vm->diag.msg[0] = '\0';
    vm->diag.msg_len = 0;
    vm->compiled = 0;
    return compile_program(vm);
}

VantaResult vanta_exec(VantaVM *vm) {
    if (!vm || !vm->compiled) return VANTA_ERR_RUNTIME;
    vm->diag.result = VANTA_OK;
    vm->diag.msg[0] = '\0';
    vm->diag.msg_len = 0;
    return interpret(vm);
}

VantaResult vanta_run(VantaVM *vm, const char *source, size_t len) {
    VantaResult r = vanta_init(vm, source, len);
    if (r != VANTA_OK) return r;
    r = vanta_compile(vm);
    return r == VANTA_OK ? vanta_exec(vm) : r;
}

VantaResult vanta_run_string(const char *source) {
    static VantaVM vm;
    if (!source) return VANTA_ERR_RUNTIME;
    return vanta_run(&vm, source, v_strlen(source));
}

VantaResult vanta_run_file(const char *filename) {
    static char source[VANTA_MAX_SRC + 1u];
    static VantaVM vm;
    if (!filename || !*filename) return VANTA_ERR_FILE_NOT_FOUND;
    int got = load_file_content(filename, source, (int)VANTA_MAX_SRC);
    if (got < 0) return VANTA_ERR_FILE_NOT_FOUND;
    if ((uint32_t)got > VANTA_MAX_SRC) return VANTA_ERR_SOURCE_TOO_LARGE;
    source[got] = '\0';
    return vanta_run(&vm, source, (size_t)got);
}

const char *vanta_last_error(VantaVM *vm) {
    if (!vm || vm->diag.result == VANTA_OK) return "";
    return vm->diag.msg;
}

void vanta_set_fuel(VantaVM *vm, uint64_t fuel) {
    if (vm) vm->fuel = fuel;
}

void vanta_set_trace(VantaVM *vm, bool on) {
    if (vm) vm->trace_on = on ? 1u : 0u;
}

void vanta_format_error(VantaVM *vm, char *buf, size_t cap) {
    if (!buf || cap == 0) return;
    if (!vm || vm->diag.result == VANTA_OK) {
        v_strlcpy(buf, "OK", cap);
        return;
    }
    vant_snprintf(buf, cap, "%s at %u:%u: %s", vanta_strerror(vm->diag.result),
                  vm->diag.line, vm->diag.col, vm->diag.msg);
}

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
} VantaOutBuf;

static void outbuf_putn(VantaOutBuf *out, const char *s, size_t n) {
    if (out->cap) {
        size_t room = out->len < out->cap - 1 ? out->cap - 1 - out->len : 0;
        size_t copy = n < room ? n : room;
        if (copy) v_memcpy(out->buf + out->len, s, copy);
    }
    out->len += n;
}

static void outbuf_printf(VantaOutBuf *out, const char *fmt, ...) {
    char temp[192];
    va_list ap;
    va_start(ap, fmt);
    int n = vanta_vsnprintf(temp, sizeof(temp), fmt, ap);
    va_end(ap);
    if (n > 0) outbuf_putn(out, temp, (size_t)n);
}

size_t vanta_disassemble(VantaVM *vm, char *buf, size_t cap) {
    VantaOutBuf out = { buf, cap, 0 };
    if (!vm || !buf || cap == 0) return 0;
    uint32_t pc = 0;
    while (pc < vm->bc_len) {
        uint32_t at = pc, op = vm->bytecode[pc++];
        if (op >= OP_COUNT || pc + op_info[op].operand_bytes > vm->bc_len) {
            outbuf_printf(&out, "%04u  <invalid bytecode>\n", at);
            break;
        }
        outbuf_printf(&out, "%04u  %-16s", at, op_info[op].name);
        if (op_info[op].operand_bytes == 4) {
            uint32_t value = (uint32_t)vm->bytecode[pc] | ((uint32_t)vm->bytecode[pc+1] << 8) | ((uint32_t)vm->bytecode[pc+2] << 16) | ((uint32_t)vm->bytecode[pc+3] << 24);
            outbuf_printf(&out, " %u", value);
        } else if (op_info[op].operand_bytes == 8) {
            int64_t value = 0;
            for (uint32_t i=0; i<8; i++) value |= (int64_t)((uint64_t)vm->bytecode[pc+i] << (i*8u));
            outbuf_printf(&out, " %lld", (long long)value);
        }
        outbuf_printf(&out, "  ; line %u\n", pc_to_line(vm, at));
        pc += op_info[op].operand_bytes;
    }
    if (cap) buf[out.len < cap ? out.len : cap - 1] = '\0';
    return out.len;
}

void vanta_dump_stats(VantaVM *vm, char *buf, size_t cap) {
    if (!buf || cap == 0) return;
    if (!vm) { v_strlcpy(buf, "Vanta: no VM", cap); return; }
    vant_snprintf(buf, cap, "Vanta %s: bytecode=%u/%u tokens=%u/%u strings=%u/%u arrays=%u/%u maps=%u/%u",
                  VANTA_VERSION_STRING, vm->bc_len, VANTA_MAX_BYTECODE, vm->token_count,
                  VANTA_MAX_TOKENS, vm->str_live, VANTA_STR_HEADS, vm->arr_live,
                  VANTA_ARR_HEADS, vm->map_live, VANTA_MAP_HEADS);
}

static int vanta_test_script(const char *source, const char *expected) {
    static VantaVM vm;
    s_capture_len = 0;
    s_capture_on = true;
    VantaResult r = vanta_run(&vm, source, v_strlen(source));
    s_capture_on = false;
    return r != VANTA_OK || !v_memcmp_eq(s_capture, expected, v_strlen(expected)) ||
           s_capture_len != v_strlen(expected);
}

int vanta_selftest(void) {
    int failed = 0;
    failed += vanta_test_script("println(2 + 3 * 4)\n", "14\n");
    failed += vanta_test_script("let a = [1, 2]\na.push(3)\nprintln(a.join())\n", "123\n");
    failed += vanta_test_script("fn fact(n) { if n <= 1 { return 1 } return n * fact(n - 1) }\nprintln(fact(6))\n", "720\n");
    failed += vanta_test_script("let n = 0\nfor let i = 0; i < 4; i++ { n += i }\nprintln(n)\n", "6\n");
    failed += vanta_test_script("let m = {name: \"vanta\"}\nprintln(m.name.upper())\n", "VANTA\n");
    return failed;
}

__attribute__((weak)) int vanta_syscall_hook(int sys_id, int64_t arg1, int64_t arg2) {
    (void)sys_id;
    (void)arg1;
    (void)arg2;
    return -1;
}
