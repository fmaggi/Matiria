#include "compiler.h"

#include "AST/symbol.h"
#include "bytecode.h"
#include "package.h"

#include "scanner/scanner.h"

#include "parser/parser.h"

#include "validator/validator.h"

#include "runtime/object.h"

#include "core/log.h"
#include "core/macros.h"

#include "debug/disassemble.h"
#include "debug/dump.h"

#include <stdlib.h>
#include <string.h>

static u64 evaluate_int(struct mtr_token token) {
    u64 s = 0;
    for (u32 i = 0; i < token.length; ++i) {
        s *= 10;
        s += token.start[i] - '0';
    }
    return s;
}

static f64 evaluate_float(struct mtr_token token) {
    f64 s = 0;
    const char* c = token.start;
    while (*c != '.') {
        s *= 10;
        s += *c - '0';
        c++;
    }

    c++;

    u64 i = 10;
    while (c != token.start + token.length) {
        f64 x = (f64)(*c - '0') / i;
        s += x;
        c++;
        i *= 10;
    }
    return s;
}

static void write_u64(struct mtr_chunk* chunk, u64 value) {
    // this is definetly dangerous, but fun :). it probably breaks for big endian
    mtr_write_chunk(chunk, (u8) (value >> 0));
    mtr_write_chunk(chunk, (u8) (value >> 8));
    mtr_write_chunk(chunk, (u8) (value >> 16));
    mtr_write_chunk(chunk, (u8) (value >> 24));
    mtr_write_chunk(chunk, (u8) (value >> 32));
    mtr_write_chunk(chunk, (u8) (value >> 40));
    mtr_write_chunk(chunk, (u8) (value >> 48));
    mtr_write_chunk(chunk, (u8) (value >> 56));
}

static void write_u32(struct mtr_chunk* chunk, u32 value) {
    mtr_write_chunk(chunk, (u8) (value >> 0));
    mtr_write_chunk(chunk, (u8) (value >> 8));
    mtr_write_chunk(chunk, (u8) (value >> 16));
    mtr_write_chunk(chunk, (u8) (value >> 24));
}

static void write_u16(struct mtr_chunk* chunk, u16 value) {
    mtr_write_chunk(chunk, (u8) (value >> 0));
    mtr_write_chunk(chunk, (u8) (value >> 8));
}

// returns the location of where to jump relative to the chunk
static u16 write_jump(struct mtr_chunk* chunk, u8 instruction) {
    mtr_write_chunk(chunk, instruction);
    write_u16(chunk, (u16) 0xFFFFu);
    return chunk->size - 2;
}

// patch the jump address after a block of bytecode
static void patch_jump(struct mtr_chunk* chunk, i16 offset) {
    i16 where = chunk->size - offset - 2;
    i16* to_patch = (i16*)(chunk->bytecode + offset);
    *to_patch = where;
}

static void write_loop(struct mtr_chunk* chunk, u16 offset) {
    mtr_write_chunk(chunk, MTR_OP_JMP);
    i16 where = offset - chunk->size - 3;
    write_u16(chunk, mtr_reinterpret_cast(u16, where));
}

static void write_expr(struct mtr_chunk* chunk, struct mtr_expr* expr);

static void write_primary(struct mtr_chunk* chunk, struct mtr_primary* expr) {
    u8 op = expr->symbol.is_global ? MTR_OP_GLOBAL_GET
        : expr->symbol.upvalue ? MTR_OP_UPVALUE_GET
        : MTR_OP_GET;
    mtr_write_chunk(chunk, op);
    write_u16(chunk, (u16)expr->symbol.index);
}

static void write_literal(struct mtr_chunk* chunk, struct mtr_literal* expr) {
    switch (expr->literal.type)
    {
    case MTR_TOKEN_INT_LITERAL: {
        mtr_write_chunk(chunk, MTR_OP_INT);
        u64 value = evaluate_int(expr->literal);
        write_u64(chunk, value);
        break;
    }

    case MTR_TOKEN_FLOAT_LITERAL: {
        mtr_write_chunk(chunk, MTR_OP_FLOAT);
        f64 value = evaluate_float(expr->literal);
        write_u64(chunk, mtr_reinterpret_cast(u64, value));
        break;
    }

    case MTR_TOKEN_STRING_LITERAL: {
        // this is weird
        mtr_write_chunk(chunk, MTR_OP_STRING_LITERAL);
        const char* string_start = expr->literal.start+1; // skip opening "
        write_u64(chunk, mtr_reinterpret_cast(u64, string_start));
        write_u32(chunk, expr->literal.length - 2); // skip closing "
        break;
    }

    case MTR_TOKEN_TRUE: {
        mtr_write_chunk(chunk, MTR_OP_TRUE);
        break;
    }

    case MTR_TOKEN_FALSE: {
        mtr_write_chunk(chunk, MTR_OP_FALSE);
        break;
    }
    default:
        MTR_LOG_WARN("Invalid type");
        break;
    }
}

static void write_array_literal(struct mtr_chunk* chunk, struct mtr_array_literal* array) {
    for (u8 i = 0; i < array->count; ++i) {
        // We need to write them from last to first to keep the array order
        // Doing the for loop that way results in unsigned int wrapping around\, so it doesnt work
        u8 actual_index = array->count - i - 1;
        write_expr(chunk, array->expressions[actual_index]);
    }

    mtr_write_chunk(chunk, MTR_OP_ARRAY_LITERAL);
    mtr_write_chunk(chunk, array->count);
}

static void write_map_literal(struct mtr_chunk* chunk, struct mtr_map_literal* map) {
    for (u8 i = 0; i < map->count; ++i) {
        u8 actual_index = map->count - i - 1;
        struct mtr_map_entry e = map->entries[actual_index];
        write_expr(chunk, e.key);
        write_expr(chunk, e.value);
    }

    mtr_write_chunk(chunk, MTR_OP_MAP_LITERAL);
    mtr_write_chunk(chunk, map->count);
}

static void write_and(struct mtr_chunk* chunk, struct mtr_binary* expr) {
    write_expr(chunk, expr->left);
    u16 offset = write_jump(chunk, MTR_OP_AND);

    write_expr(chunk, expr->right);
    patch_jump(chunk, offset);
}

static void write_or(struct mtr_chunk* chunk, struct mtr_binary* expr) {
    write_expr(chunk, expr->left);
    u16 left_true = write_jump(chunk, MTR_OP_OR);

    write_expr(chunk, expr->right);
    patch_jump(chunk, left_true);
}

static void write_binary(struct mtr_chunk* chunk, struct mtr_binary* expr) {
    // handle && and || as they are short circuited
    if (expr->operator.token.type == MTR_TOKEN_AND) {
        write_and(chunk, expr);
        return;
    } else if (expr->operator.token.type == MTR_TOKEN_OR) {
        write_or(chunk, expr);
        return;
    }

    write_expr(chunk, expr->left);
    write_expr(chunk, expr->right);

#define BINARY_OP(op)                                             \
    do {                                                          \
        if (expr->operator.type->type == MTR_DATA_INT) {           \
            mtr_write_chunk(chunk, MTR_OP_ ## op ## _I);          \
        } else if (expr->operator.type->type == MTR_DATA_FLOAT) {  \
            mtr_write_chunk(chunk, MTR_OP_ ## op ## _F);          \
        } else {                                                  \
            MTR_LOG_WARN("Invalid data type.");                   \
        }                                                         \
    } while (false)

    switch (expr->operator.token.type)
    {
    case MTR_TOKEN_PLUS:
        BINARY_OP(ADD);
        break;

    case MTR_TOKEN_MINUS:
        BINARY_OP(SUB);
        break;

    case MTR_TOKEN_STAR:
        BINARY_OP(MUL);
        break;

    case MTR_TOKEN_SLASH:
        BINARY_OP(DIV);
        break;

    case MTR_TOKEN_LESS:
        BINARY_OP(LESS);
        break;

    case MTR_TOKEN_LESS_EQUAL:
        BINARY_OP(GREATER);
        mtr_write_chunk(chunk, MTR_OP_NOT);
        break;

    case MTR_TOKEN_GREATER:
        BINARY_OP(GREATER);
        break;

    case MTR_TOKEN_GREATER_EQUAL:
        BINARY_OP(LESS);
        mtr_write_chunk(chunk, MTR_OP_NOT);
        break;

    case MTR_TOKEN_EQUAL:
        BINARY_OP(EQUAL);
        break;

    case MTR_TOKEN_BANG_EQUAL:
        BINARY_OP(EQUAL);
        mtr_write_chunk(chunk, MTR_OP_NOT);
        break;

    default:
        break;
    }

#undef BINARY_OP
}

static void write_unary(struct mtr_chunk* chunk, struct mtr_unary* unary) {
    write_expr(chunk, unary->right);

    switch (unary->operator.token.type)
    {
    case MTR_TOKEN_BANG:
        mtr_write_chunk(chunk, MTR_OP_NOT);
        break;
    case MTR_TOKEN_MINUS:
        if (unary->operator.type->type == MTR_DATA_INT) {
            mtr_write_chunk(chunk, MTR_OP_NEGATE_I);
        } else {
            mtr_write_chunk(chunk, MTR_OP_NEGATE_F);
        }
        break;
    default:
        break;
    }
}

static void write_call(struct mtr_chunk* chunk, struct mtr_call* call) {
    for (u8 i = 0; i < call->argc; ++i) {
        struct mtr_expr* expr = call->argv[i];
        write_expr(chunk, expr);
    }

    write_expr(chunk, call->callable);
    mtr_write_chunk(chunk, MTR_OP_CALL);
    mtr_write_chunk(chunk, call->argc);
}

static void write_cast(struct mtr_chunk* chunk, struct mtr_cast* cast) {
    write_expr(chunk, cast->right);

    switch (cast->to.type) {
    case MTR_DATA_FLOAT: {
        mtr_write_chunk(chunk, MTR_OP_FLOAT_CAST);
        break;
    }

    case MTR_DATA_INT: {
        mtr_write_chunk(chunk, MTR_OP_INT_CAST);
        break;
    }

    default:
        break;
    }
}

static void write_subscript(struct mtr_chunk* chunk, struct mtr_access* expr) {
    write_expr(chunk, expr->object);
    write_expr(chunk, expr->element);
    mtr_write_chunk(chunk, MTR_OP_INDEX_GET);
}

static void write_access(struct mtr_chunk* chunk, struct mtr_access* expr) {
    write_expr(chunk, expr->object);
    struct mtr_primary* p = (struct mtr_primary*) expr->element;
    mtr_write_chunk(chunk, MTR_OP_STRUCT_GET);
    write_u16(chunk, p->symbol.index);
}

static void write_expr(struct mtr_chunk* chunk, struct mtr_expr* expr) {
    switch (expr->type)
    {
    case MTR_EXPR_BINARY:  write_binary(chunk, (struct mtr_binary*) expr); return;
    case MTR_EXPR_PRIMARY: write_primary(chunk, (struct mtr_primary*) expr); return;
    case MTR_EXPR_LITERAL: write_literal(chunk, (struct mtr_literal*) expr); return;
    case MTR_EXPR_ARRAY_LITERAL: write_array_literal(chunk, (struct mtr_array_literal*) expr); return;
    case MTR_EXPR_MAP_LITERAL: write_map_literal(chunk, (struct mtr_map_literal*) expr); return;
    case MTR_EXPR_UNARY:   write_unary(chunk, (struct mtr_unary*) expr); return;
    case MTR_EXPR_GROUPING: write_expr(chunk, ((struct mtr_grouping*) expr)->expression); return;
    case MTR_EXPR_CALL: write_call(chunk, (struct mtr_call*) expr); return;
    case MTR_EXPR_CAST: write_cast(chunk, (struct mtr_cast*) expr); return;
    case MTR_EXPR_ACCESS: write_access(chunk, (struct mtr_access*) expr); return;
    case MTR_EXPR_SUBSCRIPT: write_subscript(chunk, (struct mtr_access*) expr); return;
    }
}

static void write(struct mtr_chunk* chunk, struct mtr_stmt* stmt);

static void write_variable(struct mtr_chunk* chunk, struct mtr_variable* var) {
    u8 nil_op;

    switch (var->symbol.type->type) {
    case MTR_DATA_STRING: {
        nil_op = MTR_OP_EMPTY_STRING;
        break;
    }

    case MTR_DATA_ARRAY: {
        nil_op = MTR_OP_EMPTY_ARRAY;
        break;
    }

    case MTR_DATA_MAP: {
        nil_op = MTR_OP_EMPTY_MAP;
        break;
    }

    case MTR_DATA_STRUCT: {
        nil_op = MTR_OP_NIL;
        break;
    }

    default: {
        nil_op = MTR_OP_NIL;
        break;
    }
    }

    if (NULL == var->value) {
        mtr_write_chunk(chunk, nil_op);
    } else {
        write_expr(chunk, var->value);
    }
}

static void write_block(struct mtr_chunk* chunk, struct mtr_block* stmt) {
    for (size_t i = 0; i < stmt->size; ++i) {
        struct mtr_stmt* s = stmt->statements[i];
        write(chunk, s);
    }

    mtr_write_chunk(chunk, MTR_OP_POP_V);
    write_u16(chunk, stmt->var_count);
}

static void write_if(struct mtr_chunk* chunk, struct mtr_if* stmt) {
    write_expr(chunk, stmt->condition);
    u16 offset = write_jump(chunk, MTR_OP_JMP_Z);

    write(chunk, stmt->then);

    if (stmt->otherwise) {
        u16 otherwise = write_jump(chunk, MTR_OP_JMP);
        patch_jump(chunk, offset);
        write(chunk, stmt->otherwise);
        patch_jump(chunk, otherwise);
    } else {
        patch_jump(chunk, offset);
    }
}

static void write_while(struct mtr_chunk* chunk, struct mtr_while* stmt) {
    write_expr(chunk, stmt->condition);
    u16 offset = write_jump(chunk, MTR_OP_JMP_Z);

    write(chunk, stmt->body);

    write_expr(chunk, stmt->condition); // we need to write the condition again because it was popped
    write_loop(chunk, offset);

    patch_jump(chunk, offset);
}

static void write_assignment(struct mtr_chunk* chunk, struct mtr_assignment* stmt) {
    write_expr(chunk, stmt->expression);

    switch (stmt->right->type) {
    case MTR_EXPR_PRIMARY: {
        struct mtr_primary* p = (struct mtr_primary*) stmt->right;
        u8 op = p->symbol.upvalue ? MTR_OP_UPVALUE_SET : MTR_OP_SET;
        mtr_write_chunk(chunk, op);
        write_u16(chunk, p->symbol.index);
        return;
    }
    case MTR_EXPR_SUBSCRIPT: {
        struct mtr_access* s = (struct mtr_access*) stmt->right;
        write_expr(chunk, s->object);
        write_expr(chunk, s->element);
        mtr_write_chunk(chunk, MTR_OP_INDEX_SET);
        return;
    }
    case MTR_EXPR_ACCESS: {
        struct mtr_access* s = (struct mtr_access*) stmt->right;
        write_expr(chunk, s->object);
        struct mtr_primary* p = (struct mtr_primary*) s->element;
        mtr_write_chunk(chunk, MTR_OP_STRUCT_SET);
        write_u16(chunk, p->symbol.index);
        return;
    }

    default:
        break;
    }
    MTR_ASSERT(false, "Invalid expr type.");
}

static void write_return(struct mtr_chunk* chunk, struct mtr_return* stmt) {
    if (stmt->expr) {
        write_expr(chunk, stmt->expr);
    } else {
        mtr_write_chunk(chunk, MTR_OP_NIL);
    }

    mtr_write_chunk(chunk, MTR_OP_RETURN);
}

static void write_call_stmt(struct mtr_chunk* chunk, struct mtr_call_stmt* call) {
    write_expr(chunk, call->call);
    mtr_write_chunk(chunk, MTR_OP_POP);
}

static void write_function(struct mtr_chunk* chunk, struct mtr_function_decl* fn) {
    write(chunk, fn->body);
}

static void write_closure(struct mtr_chunk* chunk, struct mtr_closure_decl* c) {
    struct mtr_chunk closure_chunk = mtr_new_chunk();
    write_function(&closure_chunk, c->function);

    struct mtr_closure* closure = mtr_new_closure(closure_chunk, NULL, c->count);

    mtr_write_chunk(chunk, MTR_OP_CLOSURE);
    write_u64(chunk, mtr_reinterpret_cast(u64, closure));

    for (u16 i = 0; i < c->count; ++i) {
        struct mtr_upvalue_symbol s = c->upvalues[i];
        write_u16(chunk, (u16)s.index);
        mtr_write_chunk(chunk, s.local);
    }
}

static void write(struct mtr_chunk* chunk, struct mtr_stmt* stmt) {
    switch (stmt->type)
    {
    case MTR_STMT_VAR:   write_variable(chunk, (struct mtr_variable*) stmt); return;

    case MTR_STMT_IF:    write_if(chunk, (struct mtr_if*) stmt); return;
    case MTR_STMT_WHILE: write_while(chunk, (struct mtr_while*) stmt); return;

    // scopes are just for validation purposes
    case MTR_STMT_SCOPE:
    case MTR_STMT_BLOCK:
        write_block(chunk, (struct mtr_block*) stmt); return;

    case MTR_STMT_ASSIGNMENT: write_assignment(chunk, (struct mtr_assignment*) stmt); return;
    case MTR_STMT_RETURN: write_return(chunk, (struct mtr_return*) stmt); return;
    case MTR_STMT_CALL: write_call_stmt(chunk, (struct mtr_call_stmt*) stmt); return;
    case MTR_STMT_CLOSURE: write_closure(chunk, (struct mtr_closure_decl*) stmt); return;

    case MTR_STMT_UNION:
    case MTR_STMT_STRUCT:
    case MTR_STMT_NATIVE_FN:
    case MTR_STMT_FN:
        return;

    }
}

static void write_struct(struct mtr_chunk* chunk, struct mtr_struct_decl* s) {
    for (u8 i = 0; i < s->argc; ++i) {
        struct mtr_variable* v = s->members[i];
        write_variable(chunk, v);
    }
    mtr_write_chunk(chunk, MTR_OP_CONSTRUCTOR);
    mtr_write_chunk(chunk, s->argc);
    mtr_write_chunk(chunk, MTR_OP_RETURN);
}

// as every function has its own chunk we could probably paralellize this pretty easily
static void write_bytecode(struct mtr_stmt* stmt, struct mtr_package* package) {
    switch (stmt->type)
    {
    case MTR_STMT_FN: {
        struct mtr_function_decl* fn = (struct mtr_function_decl*) stmt;
        struct mtr_chunk chunk = mtr_new_chunk();
        write_function(&chunk, fn);
        struct mtr_function* f = mtr_new_function(chunk);
        mtr_package_insert_function(package, (struct mtr_object*) f, fn->symbol);
        break;
    }
    case MTR_STMT_STRUCT: {
        struct mtr_struct_decl* sd = (struct mtr_struct_decl*) stmt;
        struct mtr_chunk chunk = mtr_new_chunk();
        write_struct(&chunk, sd);
        struct mtr_function* constructor = mtr_new_function(chunk);
        mtr_package_insert_function(package, (struct mtr_object*) constructor, sd->symbol);
        break;
    }
    default:
        break;
    }
}

enum mtr_exit_code mtr_compile(const char* source, struct mtr_package* package) {
    enum mtr_exit_code ec = MTR_OK;

    struct mtr_parser parser;
    mtr_parser_init(&parser, source);

    struct mtr_ast ast = mtr_parse(&parser);

    if (parser.had_error){
        ec = MTR_PARSER_ERROR;
        goto ret;
    }

    bool all_ok = mtr_validate(&ast);

    if (!all_ok) {
        ec = MTR_TYPE_ERROR; // need to handle type and scope errors separetly
        goto ret;
    }

    mtr_load_package(package, &ast);

    struct mtr_block* block = (struct mtr_block*) ast.head;
    for (size_t i = 0; i < block->size; ++i) {
        struct mtr_stmt* s = block->statements[i];
        write_bytecode(s, package);
    }

ret:
    mtr_delete_ast(&ast);
    return ec;
}
