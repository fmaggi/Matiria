#include "parser.h"

#include "AST/AST.h"
#include "AST/typeList.h"
#include "core/types.h"
#include "core/report.h"
#include "core/log.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "debug/dump.h"
#include "scanner/scanner.h"
#include "scanner/token.h"

#define ALLOCATE_EXPR(type, expr) allocate_expr(type, sizeof(struct expr))
#define ALLOCATE_STMT(type, stmt) allocate_stmt(type, sizeof(struct stmt))
#define ALLOCATE_TYPE(type, obj)  allocate_type(type, obj ? sizeof(obj) : sizeof(struct mtr_type))

static void init_block(struct mtr_block* block);
static void write_block(struct mtr_block* block, struct mtr_stmt* declaration);
static void delete_block(struct mtr_block* block);

static void* allocate_expr(enum mtr_expr_type type, size_t size) {
    struct mtr_expr* node = malloc(size);
    node->type = type;
    return node;
}

static void* allocate_stmt(enum mtr_stmt_type type, size_t size) {
    struct mtr_stmt* node = malloc(size);
    node->type = type;
    return node;
}

static void* allocate_type(enum mtr_data_type type, size_t size) {
    struct mtr_type* t = malloc(size);
    t->type = type;
    return t;
}

static void parser_error(struct mtr_parser* parser, const char* message) {
    parser->had_error = true;
    if (!parser->panic)
        mtr_report_error(parser->token, message, parser->scanner.source);
    parser->panic = true;
}

#define CHECK(token_type) (parser->token.type == token_type)

static struct mtr_token advance(struct mtr_parser* parser) {
    struct mtr_token previous = parser->token;

    parser->token = mtr_next_token(&parser->scanner);

    while (CHECK(MTR_TOKEN_COMMENT))
        parser->token = mtr_next_token(&parser->scanner);

    while (CHECK(MTR_TOKEN_INVALID)) {
        parser_error(parser, "Invalid token.");
        parser->token = mtr_next_token(&parser->scanner);
    }

    return previous;
}

static struct mtr_token peek(struct mtr_parser* parser) {
    struct mtr_scanner dummy = parser->scanner;
    struct mtr_token token = mtr_next_token(&dummy);

    while(CHECK(MTR_TOKEN_COMMENT) || CHECK(MTR_TOKEN_INVALID))
        token = mtr_next_token(&dummy);

    return token;
}

static struct mtr_token consume(struct mtr_parser* parser, enum mtr_token_type token, const char* message) {
    if (parser->token.type == token)
        return advance(parser);

    parser_error(parser, message);
    return invalid_token;
}

void mtr_parser_init(struct mtr_parser* parser, const char* source) {
    mtr_scanner_init(&parser->scanner, source);
    parser->current_function = NULL;
    parser->had_error = false;
    parser->panic = false;
}

static void synchronize(struct mtr_parser* parser) {
    if (!parser->panic)
        return;

    parser->panic = false;
    while (!CHECK(MTR_TOKEN_EOF)) {
        switch (parser->token.type)
        {
        case MTR_TOKEN_INT:
        case MTR_TOKEN_FLOAT:
        case MTR_TOKEN_BOOL:
        case MTR_TOKEN_FN:
        case MTR_TOKEN_IF:
        case MTR_TOKEN_WHILE:
        case MTR_TOKEN_CURLY_L:
        case MTR_TOKEN_CURLY_R:
            return;
        default:
            advance(parser);
        }
    }
}

// ======================== EXPR =============================

static struct mtr_expr* expression(struct mtr_parser* parser);

enum precedence {
    NONE,
    LOGIC,
    EQUALITY,
    COMPARISON,
    TERM,
    FACTOR,
    UNARY,
    CALL,
    SUB,
    ACCESS,
    PRIMARY,
    LITERAL = PRIMARY
};

typedef struct mtr_expr* (*prefix_fn)(struct mtr_parser*, struct mtr_token);
typedef struct mtr_expr* (*infix_fn)(struct mtr_parser*, struct mtr_token, struct mtr_expr*);

struct parser_rule {
    prefix_fn prefix;
    infix_fn infix;
    enum precedence precedence;
};

static struct mtr_expr* unary(struct mtr_parser* parser, struct mtr_token op);
static struct mtr_expr* binary(struct mtr_parser* parser, struct mtr_token op, struct mtr_expr* left);
static struct mtr_expr* grouping(struct mtr_parser* parser, struct mtr_token token);
static struct mtr_expr* primary(struct mtr_parser* parser, struct mtr_token primary);
static struct mtr_expr* literal(struct mtr_parser* parser, struct mtr_token literal);
static struct mtr_expr* array_literal(struct mtr_parser* parser, struct mtr_token paren);
static struct mtr_expr* map_literal(struct mtr_parser* parser, struct mtr_token paren);
static struct mtr_expr* call(struct mtr_parser* parser, struct mtr_token paren, struct mtr_expr* name);
static struct mtr_expr* subscript(struct mtr_parser* parser, struct mtr_token square, struct mtr_expr* object);
static struct mtr_expr* access(struct mtr_parser* parser, struct mtr_token dot, struct mtr_expr* object);

#define NO_OP .prefix = NULL, .infix = NULL, .precedence = NONE

static const struct parser_rule rules[] = {
    [MTR_TOKEN_PLUS] = { .prefix = NULL, .infix = binary, .precedence = TERM },
    [MTR_TOKEN_MINUS] = { .prefix = unary, .infix = binary, .precedence = TERM },
    [MTR_TOKEN_STAR] = { .prefix = NULL, .infix = binary, .precedence = FACTOR },
    [MTR_TOKEN_SLASH] = { .prefix = NULL, .infix = binary, .precedence = FACTOR },
    [MTR_TOKEN_PERCENT] = { .prefix = NULL, .infix = binary, .precedence = FACTOR },
    [MTR_TOKEN_COMMA] = { .prefix = NULL, .infix = NULL, .precedence = NONE },
    [MTR_TOKEN_COLON] = { .prefix = NULL, .infix = NULL, .precedence = NONE },
    [MTR_TOKEN_SEMICOLON] = { .prefix = NULL, .infix = NULL, .precedence = NONE },
    [MTR_TOKEN_DOT] = { .prefix = NULL, .infix = access, .precedence = ACCESS },
    [MTR_TOKEN_PAREN_L] = { .prefix = grouping, .infix = call, .precedence = CALL },
    [MTR_TOKEN_PAREN_R] = { NO_OP },
    [MTR_TOKEN_SQR_L] = { .prefix = array_literal, .infix = subscript, .precedence = SUB },
    [MTR_TOKEN_SQR_R] = { NO_OP },
    [MTR_TOKEN_CURLY_L] = { .prefix = map_literal, .infix = NULL, .precedence = NONE },
    [MTR_TOKEN_CURLY_R] = { NO_OP },
    [MTR_TOKEN_BANG] = { .prefix = unary, .infix = NULL, .precedence = UNARY },
    [MTR_TOKEN_ASSIGN] = { NO_OP },
    [MTR_TOKEN_GREATER] = { .prefix = NULL, .infix = binary, .precedence = COMPARISON },
    [MTR_TOKEN_LESS] = { .prefix = NULL, .infix = binary, .precedence = COMPARISON },
    [MTR_TOKEN_ARROW] = { NO_OP },
    [MTR_TOKEN_BANG_EQUAL] = { .prefix = NULL, .infix = binary, .precedence = EQUALITY },
    [MTR_TOKEN_EQUAL] = { .prefix = NULL, .infix = binary, .precedence = EQUALITY },
    [MTR_TOKEN_GREATER_EQUAL] = { .prefix = NULL, .infix = binary, .precedence = COMPARISON },
    [MTR_TOKEN_LESS_EQUAL] = { .prefix = NULL, .infix = binary, .precedence = COMPARISON },
    [MTR_TOKEN_DOUBLE_SLASH] = { .prefix = NULL, .infix = binary, .precedence = FACTOR },
    [MTR_TOKEN_STRING_LITERAL] = { .prefix = literal, .infix = NULL, .precedence = LITERAL },
    [MTR_TOKEN_INT_LITERAL] = { .prefix = literal, .infix = NULL, .precedence = LITERAL },
    [MTR_TOKEN_FLOAT_LITERAL] = { .prefix = literal, .infix = NULL, .precedence = LITERAL },
    [MTR_TOKEN_AND] = { .prefix = NULL, .infix = binary, .precedence = LOGIC },
    [MTR_TOKEN_OR] = { .prefix = NULL, .infix = binary, .precedence = LOGIC },
    [MTR_TOKEN_PIPE] = {NO_OP},
    [MTR_TOKEN_ELLIPSIS] = { NO_OP },
    [MTR_TOKEN_TYPE] = { NO_OP },
    [MTR_TOKEN_IF] = { NO_OP },
    [MTR_TOKEN_ELSE] = { NO_OP },
    [MTR_TOKEN_TRUE] = { .prefix = literal, .infix = NULL, .precedence = LITERAL },
    [MTR_TOKEN_FALSE] = { .prefix = literal, .infix = NULL, .precedence = LITERAL },
    [MTR_TOKEN_FN] = { NO_OP },
    [MTR_TOKEN_RETURN] = { NO_OP },
    [MTR_TOKEN_WHILE] = { NO_OP },
    [MTR_TOKEN_FOR] = { NO_OP },
    [MTR_TOKEN_INT] = { NO_OP },
    [MTR_TOKEN_FLOAT] = { NO_OP },
    [MTR_TOKEN_BOOL] = { NO_OP },
    [MTR_TOKEN_IDENTIFIER] = { .prefix = primary, .infix = NULL, .precedence = PRIMARY },
    [MTR_TOKEN_COMMENT] = { NO_OP },
    [MTR_TOKEN_EOF] = { NO_OP },
    [MTR_TOKEN_INVALID] = { NO_OP }
};

#undef NO_OP

static struct mtr_expr* parse_precedence(struct mtr_parser* parser, enum precedence precedece) {
    struct mtr_expr* node = NULL;

    {
        struct mtr_token token = advance(parser);
        prefix_fn prefix = rules[token.type].prefix;
        if (NULL == prefix) {
            parser_error(parser, "Expected expression.");
            return NULL;
        }
        node = prefix(parser, token);
    }

    while (precedece <= rules[parser->token.type].precedence && rules[parser->token.type].infix) {
        struct mtr_token token = advance(parser);
        infix_fn infix = rules[token.type].infix;
        node = infix(parser, token, node);
    }

    return node;
}

static struct mtr_expr* unary(struct mtr_parser* parser, struct mtr_token op) {
    struct mtr_unary* node = ALLOCATE_EXPR(MTR_EXPR_UNARY, mtr_unary);
    node->operator.token = op;
    node->right = parse_precedence(parser, rules[op.type].precedence + 1);
    return (struct mtr_expr*) node;
}

static struct mtr_expr* binary(struct mtr_parser* parser, struct mtr_token op, struct mtr_expr* left) {
    struct mtr_binary* node = ALLOCATE_EXPR(MTR_EXPR_BINARY, mtr_binary);
    node->left = left;
    node->operator.token = op;
    node->right = parse_precedence(parser, rules[op.type].precedence + 1);
    return (struct mtr_expr*) node;
}

static struct mtr_expr* grouping(struct mtr_parser* parser, struct mtr_token token) {
    struct mtr_grouping* node = ALLOCATE_EXPR(MTR_EXPR_GROUPING, mtr_grouping);
    node->expression = expression(parser);
    consume(parser, MTR_TOKEN_PAREN_R, "Expected ')'.");
    return (struct mtr_expr*) node;
}

static struct mtr_expr* primary(struct mtr_parser* parser, struct mtr_token primary) {
    struct mtr_primary* node = ALLOCATE_EXPR(MTR_EXPR_PRIMARY, mtr_primary);
    node->symbol.token = primary;
    node->symbol.type = NULL;
    return (struct mtr_expr*) node;
}

static struct mtr_expr* literal(struct mtr_parser* parser, struct mtr_token literal) {
    struct mtr_literal* node = ALLOCATE_EXPR(MTR_EXPR_LITERAL, mtr_literal);
    node->literal = literal;
    return (struct mtr_expr*) node;
}

static struct mtr_expr* array_literal(struct mtr_parser* parser, struct mtr_token paren) {
    struct mtr_array_literal* node = ALLOCATE_EXPR(MTR_EXPR_ARRAY_LITERAL, mtr_array_literal);

    u8 count = 0;
    struct mtr_expr* exprs[255];
    bool cont = true;
    while (count < 255 && cont) {
        exprs[count++] = expression(parser);
        if (CHECK(MTR_TOKEN_SQR_R)) {
            advance(parser);
            break;
        }

        cont = consume(parser, MTR_TOKEN_COMMA, "Expected ','.").type == MTR_TOKEN_COMMA; // this is ugly as fuck
    }

    node->count = count;
    node->expressions = malloc(sizeof(struct mtr_expr*) * count);
    memcpy(node->expressions, exprs, sizeof(struct mtr_expr*) * count);

    return (struct mtr_expr*) node;
}

static struct mtr_expr* map_literal(struct mtr_parser* parser, struct mtr_token paren) {
    struct mtr_map_literal* node = ALLOCATE_EXPR(MTR_EXPR_MAP_LITERAL, mtr_map_literal);

    u8 count = 0;
    struct mtr_map_entry entries[255];
    bool cont = true;
    while (count < 255 && cont) {
        entries[count].key = expression(parser);
        consume(parser, MTR_TOKEN_COLON, "Expected ':'.");
        entries[count++].value = expression(parser);

        if (CHECK(MTR_TOKEN_CURLY_R)) {
            advance(parser);
            break;
        }

        cont = consume(parser, MTR_TOKEN_COMMA, "Expected ','.").type == MTR_TOKEN_COMMA; // this is ugly as fuck
    }

    node->count = count;
    node->entries = malloc(sizeof(struct mtr_map_entry) * count);
    memcpy(node->entries, entries, sizeof(struct mtr_map_entry) * count);

    return (struct mtr_expr*) node;
}

static struct mtr_expr* call(struct mtr_parser* parser, struct mtr_token paren, struct mtr_expr* name) {
    struct mtr_call* node = ALLOCATE_EXPR(MTR_EXPR_CALL, mtr_call);
    node->callable = name;
    node->argc = 0;
    node->argv = NULL;

    if (CHECK(MTR_TOKEN_PAREN_R)) {
        // skip args because function has no params
        advance(parser);
        return (struct mtr_expr*) node;
    }

    u8 argc = 0;
    struct mtr_expr* exprs[255];
    bool cont = true;
    while (argc < 255 && cont) {
        exprs[argc++] = expression(parser);
        if (CHECK(MTR_TOKEN_PAREN_R)) {
            advance(parser);
            break;
        }
        cont = consume(parser, MTR_TOKEN_COMMA, "Expected ','.").type == MTR_TOKEN_COMMA;
    }

    node->argc = argc;
    node->argv = malloc(sizeof(struct mtr_expr*) * argc);
    memcpy(node->argv, exprs, sizeof(struct mtr_expr*) * argc);

    return (struct mtr_expr*) node;
}

static struct mtr_expr* subscript(struct mtr_parser* parser, struct mtr_token square, struct mtr_expr* object) {
    struct mtr_access* node = ALLOCATE_EXPR(MTR_EXPR_SUBSCRIPT, mtr_access);
    node->object = object;
    node->element = expression(parser);
    consume(parser, MTR_TOKEN_SQR_R, "Expected ']'.");
    return (struct mtr_expr*) node;
}

static struct mtr_expr* access(struct mtr_parser* parser, struct mtr_token dot, struct mtr_expr* object) {
    struct mtr_access* node = ALLOCATE_EXPR(MTR_EXPR_ACCESS, mtr_access);
    node->object = object;
    node->element = parse_precedence(parser, ACCESS);
    return (struct mtr_expr*) node;
}

static struct mtr_expr* expression(struct mtr_parser* parser) {
    return parse_precedence(parser, LOGIC);
}

// ============================ STMT =====================================

static struct mtr_type* parse_var_type(struct mtr_parser* parser);

static struct mtr_type* array_or_map(struct mtr_parser* parser) {
    struct mtr_type* type1 = parse_var_type(parser);

    if (CHECK(MTR_TOKEN_COMMA)) {
        advance(parser);
        struct mtr_type* type2 = parse_var_type(parser);
        return mtr_type_list_register_map(parser->type_list, type1, type2);
    }

    return mtr_type_list_register_array(parser->type_list, type1);
}

static struct mtr_type* function_type(struct mtr_parser* parser) {
    u8 argc = 0;
    struct mtr_type* types[255];
    bool cont = true;
    if (CHECK(MTR_TOKEN_PAREN_R)) {
        advance(parser);
        goto ret;
    }

    while (argc < 255 && cont) {
        types[argc++] = parse_var_type(parser);
        if (CHECK(MTR_TOKEN_PAREN_R)) {
            advance(parser);
            break;
        }

        cont = consume(parser, MTR_TOKEN_COMMA, "Expected ','.").type == MTR_TOKEN_COMMA;
    }

    if (argc > 255) {
        parser_error(parser, "Exceded maximum number of arguments (255)");
        return NULL;
    }
ret:;
    struct mtr_type* return_type = mtr_type_list_get_void_type(parser->type_list);
    if (CHECK(MTR_TOKEN_ARROW)) {
        advance(parser);
        return_type = parse_var_type(parser);
    }

    return mtr_type_list_register_function(parser->type_list, return_type, types, argc);
}

static struct mtr_type* parse_var_type(struct mtr_parser* parser) {
    switch (parser->token.type) {

    case MTR_TOKEN_ANY:
    case MTR_TOKEN_INT:
    case MTR_TOKEN_FLOAT:
    case MTR_TOKEN_BOOL:
    case MTR_TOKEN_STRING: {
        struct mtr_token token = advance(parser);
        return mtr_type_list_register_from_token(parser->type_list, token);
    }

    case MTR_TOKEN_SQR_L: {
        advance(parser);
        struct mtr_type* type = array_or_map(parser);
        consume(parser, MTR_TOKEN_SQR_R, "Expected ']'.");
        return type;
    }

    case MTR_TOKEN_PAREN_L: {
        advance(parser);
        return function_type(parser);
    }

    case MTR_TOKEN_IDENTIFIER: {
        struct mtr_token token = advance(parser);
        struct mtr_type* type = mtr_type_list_get_user_type(parser->type_list, token);
        if (type == NULL) {
            parser_error(parser, "Unknown type!");
        }
        return type;
    }

    default: {
        parser_error(parser, "Expected a type expression.");
        break;
    }

    }

    return NULL;
}

static struct mtr_stmt* declaration(struct mtr_parser* parser);

static struct mtr_stmt* expr_stmt(struct mtr_parser* parser) {
    struct mtr_stmt* node = NULL;
    struct mtr_expr* expr = expression(parser);
    switch (expr->type) {
    case MTR_EXPR_ACCESS:
    case MTR_EXPR_SUBSCRIPT:
    case MTR_EXPR_PRIMARY: {
        struct mtr_assignment* a = ALLOCATE_STMT(MTR_STMT_ASSIGNMENT, mtr_assignment);
        a->right = expr;
        consume(parser, MTR_TOKEN_ASSIGN, "Expected ':='.");
        a->expression = expression(parser);
        node = (struct mtr_stmt*) a;
        break;
    }
    case MTR_EXPR_CALL: {
        struct mtr_call_stmt* c = ALLOCATE_STMT(MTR_STMT_CALL, mtr_call_stmt);
        c->call = expr;
        node = (struct mtr_stmt*) c;
        break;
    }
    default: {
        parser_error(parser, "Expression has no effect.");
        break;
    }
    }

    consume(parser, MTR_TOKEN_SEMICOLON, "Expected ';'.");
    return node;
}

static struct mtr_stmt* block(struct mtr_parser* parser) {
    struct mtr_block* node = ALLOCATE_STMT(MTR_STMT_BLOCK, mtr_block);
    init_block(node);

    consume(parser, MTR_TOKEN_CURLY_L, "Expected '{'.");
    while(!CHECK(MTR_TOKEN_CURLY_R) && !CHECK(MTR_TOKEN_EOF)) {
        struct mtr_stmt* s = declaration(parser);
        synchronize(parser);
        write_block(node, s);
    }
    consume(parser, MTR_TOKEN_CURLY_R, "Expected '}'.");

    return (struct mtr_stmt*) node;
}

static struct mtr_stmt* scope(struct mtr_parser* parser) {
    struct mtr_block* self = (struct mtr_block*) block(parser);
    self->stmt.type = MTR_STMT_SCOPE;
    return (struct mtr_stmt*) self;
}

static struct mtr_stmt* if_stmt(struct mtr_parser* parser) {
    struct mtr_if* node = ALLOCATE_STMT(MTR_STMT_IF, mtr_if);

    advance(parser);
    node->condition = expression(parser);
    consume(parser, MTR_TOKEN_COLON, "Expected ':'.");

    if (CHECK(MTR_TOKEN_CURLY_L)) {
        node->then = block(parser);
    } else {
        node->then = declaration(parser);
    }

    node->otherwise = NULL;

    if (CHECK(MTR_TOKEN_ELSE)) {
        advance(parser);
        if (CHECK(MTR_TOKEN_CURLY_L)) {
            node->otherwise = block(parser);
        } else {
            node->otherwise = declaration(parser);
        }
    }

    return (struct mtr_stmt*) node;
}

static struct mtr_stmt* while_stmt(struct mtr_parser* parser) {
    struct mtr_while* node = ALLOCATE_STMT(MTR_STMT_WHILE, mtr_while);

    advance(parser);
    node->condition = expression(parser);
    consume(parser, MTR_TOKEN_COLON, "Expected ':'.");
    if (CHECK(MTR_TOKEN_CURLY_L)) {
        node->body = block(parser);
    } else {
        node->body = declaration(parser);
    }

    return (struct mtr_stmt*) node;
}

static struct mtr_stmt* return_stmt(struct mtr_parser* parser) {
    struct mtr_return* node = ALLOCATE_STMT(MTR_STMT_RETURN, mtr_return);
    node->expr = NULL;
    advance(parser);
    if (CHECK(MTR_TOKEN_SEMICOLON)) {
        return (struct mtr_stmt*) node;
    }
    node->expr = expression(parser);
    consume(parser, MTR_TOKEN_SEMICOLON, "Expected ';'.");

    node->from = parser->current_function;

    return (struct mtr_stmt*) node;
}

static struct mtr_stmt* statement(struct mtr_parser* parser) {
    switch (parser->token.type)
    {
    case MTR_TOKEN_IF:      return if_stmt(parser);
    case MTR_TOKEN_WHILE:   return while_stmt(parser);
    case MTR_TOKEN_CURLY_L: return scope(parser);
    case MTR_TOKEN_RETURN:  return return_stmt(parser);
    default:
        return expr_stmt(parser);
    }
}

static struct mtr_stmt* variable(struct mtr_parser* parser) {
    struct mtr_variable* node = ALLOCATE_STMT(MTR_STMT_VAR, mtr_variable);

    node->symbol.type = parse_var_type(parser);
    struct mtr_function_type* f = (struct mtr_function_type*) node->symbol.type;
    (void) f;

    node->symbol.token = consume(parser, MTR_TOKEN_IDENTIFIER, "Expected identifier.");
    node->value = NULL;

    if (CHECK(MTR_TOKEN_ASSIGN)) {
        advance(parser);
        node->value = expression(parser);
    }

    consume(parser, MTR_TOKEN_SEMICOLON, "Expected ';' or ':='.");

    return (struct mtr_stmt*) node;
}

static struct mtr_stmt* func_decl(struct mtr_parser* parser) {
    struct mtr_function_decl* node = ALLOCATE_STMT(MTR_STMT_FN, mtr_function_decl);
    parser->current_function = node;

    advance(parser);

    node->symbol.token = consume(parser, MTR_TOKEN_IDENTIFIER, "Expected identifier.");
    consume(parser, MTR_TOKEN_PAREN_L, "Expected '('.");

    node->argc = 0;
    node->argv = NULL;

    u32 argc = 0;
    struct mtr_variable vars[255];
    struct mtr_type* types[255];

    if (CHECK(MTR_TOKEN_PAREN_R)) {
        advance(parser);
        goto type_check;
    }

    bool cont = true;
    while (argc < 255 && cont) {
        types[argc] = parse_var_type(parser);
        struct mtr_variable* var = vars + argc++;
        var->stmt.type = MTR_STMT_VAR;
        var->symbol.type = types[argc-1];
        var->symbol.token = consume(parser, MTR_TOKEN_IDENTIFIER, "Expected identifier.");
        var->value = NULL;

        if (CHECK(MTR_TOKEN_PAREN_R)) {
            advance(parser);
            break;
        }

        cont = consume(parser, MTR_TOKEN_COMMA, "Expected ','.").type == MTR_TOKEN_COMMA;
    }

    if (argc > 255) {
        parser_error(parser, "Exceded maximum number of arguments (255)");
    }

    // because we are here we now that argc > 0
    node->argc = argc;
    node->argv = malloc(sizeof(struct mtr_variable) * argc);
    memcpy(node->argv, vars, sizeof(struct mtr_variable) * argc);

type_check:; // this is some weird shit with labels. prob a clang bug

    struct mtr_type* return_type = mtr_type_list_get_void_type(parser->type_list);
    if (CHECK(MTR_TOKEN_ARROW)) {
        advance(parser);
        return_type = parse_var_type(parser);
    }

    node->symbol.type = mtr_type_list_register_function(parser->type_list, return_type, types, argc);

    node->body = NULL;
    if (CHECK(MTR_TOKEN_ELLIPSIS)) {
        node->stmt.type = MTR_STMT_NATIVE_FN;
        advance(parser);
        return (struct mtr_stmt*) node;
    } else if (CHECK(MTR_TOKEN_ASSIGN)) {
        advance(parser);
        struct mtr_return* r = ALLOCATE_STMT(MTR_STMT_RETURN, mtr_return);
        r->expr = expression(parser);
        consume(parser, MTR_TOKEN_SEMICOLON, "Expected ';'.");
        r->from = node;
        node->body = (struct mtr_stmt*) r;
    } else {
        node->body = block(parser);
    }


    return (struct mtr_stmt*) node;
}

static struct mtr_stmt* closure(struct mtr_parser* parser) {
    struct mtr_closure_decl* closure = ALLOCATE_STMT(MTR_STMT_CLOSURE, mtr_closure_decl);

    struct mtr_function_decl* current_function = parser->current_function;

    struct mtr_stmt* fn = func_decl(parser);
    if (fn->type == MTR_STMT_NATIVE_FN) {
        parser_error(parser, "Closures cannot be native functions.");
        mtr_free_stmt(fn);
        free(closure);
        return NULL;
    }

    closure->function = (struct mtr_function_decl*) fn;
    closure->upvalues = NULL;
    closure->capacity = 0;
    closure->count = 0;

    parser->current_function = current_function;

    return (struct mtr_stmt*) closure;
}

static struct mtr_stmt* union_type(struct mtr_parser* parser, struct mtr_token name) {
    advance(parser);

    struct mtr_union_decl* union_ = ALLOCATE_STMT(MTR_STMT_UNION, mtr_union_decl);
    union_->symbol.token = name;

    struct mtr_type* types[UINT16_MAX];
    u16 argc = 0;
    bool cont = true;
    while (argc < UINT16_MAX && cont) {
        types[argc++] = parse_var_type(parser);

        if (CHECK(MTR_TOKEN_SQR_R)) {
            advance(parser);
            break;
        }

        cont = consume(parser, MTR_TOKEN_PIPE, "Expected '|'.").type == MTR_TOKEN_PIPE;
    }

    if (argc == 0) {
        parser_error(parser, "Unions cannot be empty.");
    }

    if (argc > 255) {
        parser_error(parser, "Exceded maximum number of types.");
    }

    union_->symbol.type = mtr_type_list_register_union_type(parser->type_list, name, types, argc);

    return (struct mtr_stmt*) union_;
}

static struct mtr_stmt* struct_type(struct mtr_parser* parser, struct mtr_token name) {
    advance(parser);

    struct mtr_struct_decl* struct_ = ALLOCATE_STMT(MTR_STMT_STRUCT, mtr_struct_decl);
    struct_->symbol.token = name;

    struct mtr_variable* vars[UINT16_MAX];
    struct mtr_symbol* symbols[UINT16_MAX];
    u16 argc = 0;
    bool cont = true;
    while (argc < UINT16_MAX && cont) {
        struct mtr_variable** type = vars + argc;
        struct mtr_symbol** symbol = symbols + argc++;
        *type = (struct mtr_variable*) variable(parser);
        *symbol = &(*type)->symbol;
        if (CHECK(MTR_TOKEN_CURLY_R)) {
            advance(parser);
            break;
        }

        cont = consume(parser, MTR_TOKEN_COMMA, "Expected ','.").type == MTR_TOKEN_COMMA;
    }

    if (argc == 0) {
        parser_error(parser, "Structs cannot be empty.");
    }

    if (argc > 255) {
        parser_error(parser, "Exceded maximum number of members.");
    }

    struct_->members = malloc(sizeof(struct mtr_variable*) * argc);
    memcpy(struct_->members, vars, sizeof(struct mtr_variable*) * argc);
    struct_->argc = argc;

    struct_->symbol.type = mtr_type_list_register_struct_type(parser->type_list, name, symbols, argc);

    return (struct mtr_stmt*) struct_;
}

static struct mtr_stmt* type(struct mtr_parser* parser) {
    advance(parser);
    struct mtr_token token = consume(parser, MTR_TOKEN_IDENTIFIER, "Expected identifier.");

    consume(parser, MTR_TOKEN_ASSIGN, "Expected ':='.");

    if (CHECK(MTR_TOKEN_SQR_L)) {
        return union_type(parser, token);
    } else if (CHECK(MTR_TOKEN_CURLY_L)) {
        return struct_type(parser, token);
    }
    parser_error(parser, "Expected either '[' or '{'.");
    return NULL;
}

static struct mtr_stmt* declaration(struct mtr_parser* parser) {
    switch (parser->token.type)
    {
    case MTR_TOKEN_IDENTIFIER: {
        struct mtr_token next_token = peek(parser);
        if (next_token.type != MTR_TOKEN_IDENTIFIER) {
            return statement(parser);
        }
        // fallthrough to variable
    }
    case MTR_TOKEN_INT:
    case MTR_TOKEN_FLOAT:
    case MTR_TOKEN_BOOL:
    case MTR_TOKEN_STRING:
    case MTR_TOKEN_SQR_L:
    case MTR_TOKEN_PAREN_L:
        return variable(parser);
    case MTR_TOKEN_ANY: {
        parser_error(parser, "'Any' expressions are only allowed as parameters to native functions.");
        exit(-1);
    }
    case MTR_TOKEN_FN: {
        return closure(parser);
    }
    default:
        return statement(parser);
    }
}

static struct mtr_stmt* global_declaration(struct mtr_parser* parser) {
    switch (parser->token.type)
    {
    case MTR_TOKEN_FN: return func_decl(parser);
    case MTR_TOKEN_TYPE: return type(parser);
    default:
        break;
    }
    parser_error(parser, "Expected function declaration.");
    exit(-1);
}

#undef CHECK

// ========================================================================

struct mtr_ast mtr_parse(struct mtr_parser* parser) {
    advance(parser);

    struct mtr_ast ast;
    struct mtr_block* block = ALLOCATE_STMT(MTR_STMT_BLOCK, mtr_block);
    ast.head = (struct mtr_stmt*) block;
    ast.source = parser->scanner.source;
    init_block(block);
    mtr_type_list_init(&ast.type_list);

    parser->type_list = &ast.type_list;
    while (parser->token.type != MTR_TOKEN_EOF) {
        struct mtr_stmt* stmt = global_declaration(parser);
        if (NULL == stmt) {
            return ast;
        }
        synchronize(parser);
        write_block(block, stmt);
    }

    return ast;
}

// =======================================================================

void mtr_delete_ast(struct mtr_ast* ast) {
    delete_block((struct mtr_block*) ast->head);
    mtr_type_list_delete(&ast->type_list);
    ast->head = NULL;
}

static void init_block(struct mtr_block* block) {
    void* temp = malloc(sizeof(struct mtr_stmt*) * 8);
    block->capacity = 8;
    block->size = 0;
    block->statements = temp;
}

static void write_block(struct mtr_block* block, struct mtr_stmt* statement) {
    if (block->size == block->capacity) {
        size_t new_cap = block->capacity * 2;
        block->statements = realloc(block->statements, new_cap * sizeof(struct mtr_stmt*));
        block->capacity = new_cap;
    }
    block->statements[block->size++] = statement;
}

static void delete_block(struct mtr_block* block) {
    for (size_t i = 0; i < block->size; i++) {
        struct mtr_stmt* s = block->statements[i];
        mtr_free_stmt(s);
    }

    free(block->statements);
    block->statements = NULL;
    block->size = 0;
    block->capacity = 0;
    free(block);
}

// =======================================================================

void mtr_free_stmt(struct mtr_stmt* s) {
    if (s == NULL) {
        MTR_LOG_DEBUG("Freeing NULL stmt.");
        return;
    }
    switch (s->type) {
        case MTR_STMT_SCOPE:
        case MTR_STMT_BLOCK: {
            delete_block((struct mtr_block*) s);
            break;
        }
        case MTR_STMT_ASSIGNMENT: {
            struct mtr_assignment* a = (struct mtr_assignment*) s;
            mtr_free_expr(a->right);
            mtr_free_expr(a->expression);
            a->right = NULL;
            a->expression = NULL;
            free(a);
            break;
        }
        case MTR_STMT_CLOSURE: {
            struct mtr_closure_decl* c = (struct mtr_closure_decl*) s;
            mtr_free_stmt((struct mtr_stmt*) c->function);
            free(c->upvalues);
            c->upvalues = NULL;
            c->count = 0;
            c->capacity = 0;
            free(c);
            break;
        }
        case MTR_STMT_NATIVE_FN:
        case MTR_STMT_FN: {
            struct mtr_function_decl* f = (struct mtr_function_decl*) s;
            for (u8 i = 0; i < f->argc; ++i) {
                struct mtr_variable* v = f->argv + i;
                if (v->value) {
                    mtr_free_expr(v->value);
                }
            }
            free(f->argv);
            if (f->body) {
                mtr_free_stmt(f->body);
            }
            f->argv = NULL;
            f->argc = 0;
            f->body = NULL;
            free(f);
            break;
        }
        case MTR_STMT_UNION: {
            struct mtr_union_decl* u = (struct mtr_union_decl*) s;
            free(u);
            break;
        }
        case MTR_STMT_STRUCT: {
            struct mtr_struct_decl* st = (struct mtr_struct_decl*) s;
            for (u8 i = 0; i < st->argc; ++i) {
                mtr_free_stmt((struct mtr_stmt*) st->members[i]);
            }
            free(st->members);
            free(st);
            break;
        }
        case MTR_STMT_IF: {
            struct mtr_if* i = (struct mtr_if*) s;
            mtr_free_stmt(i->then);
            if (i->otherwise)
                mtr_free_stmt(i->otherwise);
            mtr_free_expr(i->condition);
            i->otherwise = NULL;
            i->condition = NULL;
            free(i);
            break;
        }
        case MTR_STMT_WHILE: {
            struct mtr_while* w = (struct mtr_while*) s;
            mtr_free_expr(w->condition);
            mtr_free_stmt(w->body);
            w->body = NULL;
            w->condition = NULL;
            free(w);
            break;
        }
        case MTR_STMT_VAR: {
            struct mtr_variable* v = (struct mtr_variable*) s;
            if (v->value)
                mtr_free_expr(v->value);
            v->value = NULL;
            free(v);
            break;
        }

        case MTR_STMT_RETURN: {
            struct mtr_return* r = (struct mtr_return*) s;
            if (r->expr) {
                mtr_free_expr(r->expr);
            }
            r->expr = NULL;
            r->from = NULL;
            free(r);
            break;
        }

        case MTR_STMT_CALL: {
            struct mtr_call_stmt* c = (struct mtr_call_stmt*) s;
            mtr_free_expr(c->call);
            c->call = NULL;
            free(c);
            break;
        }
    }
}

static void free_binary(struct mtr_binary* node) {
    mtr_free_expr(node->left);
    mtr_free_expr(node->right);
    node->left = NULL;
    node->right = NULL;
    free(node);
}

static void free_grouping(struct mtr_grouping* node) {
    mtr_free_expr(node->expression);
    node->expression = NULL;
    free(node);
}

static void free_primary(struct mtr_primary* node) {
    // primary symbol type is freed by its declaration
    free(node);
}

static void free_unary(struct mtr_unary* node) {
    mtr_free_expr(node->right);
    node->right = NULL;
    free(node);
}

static void free_literal(struct mtr_literal* node) {
    free(node);
}

static void free_array_lit(struct mtr_array_literal* node) {
    for (u8 i = 0; i < node->count; ++i) {
        mtr_free_expr(node->expressions[i]);
    }
    free(node->expressions);
    node->expressions = NULL;
    free(node);
}

static void free_map_lit(struct mtr_map_literal* node) {
    for (u8 i = 0; i < node->count; ++i) {
        mtr_free_expr(node->entries[i].key);
        mtr_free_expr(node->entries[i].value);
    }
    free(node->entries);
    node->entries = NULL;
    free(node);
}

static void free_call(struct mtr_call* node) {
    if (node->argc > 0) {
        for (u8 i = 0; i < node->argc; ++i) {
            mtr_free_expr(node->argv[i]);
        }
    }
    free(node->argv);
    mtr_free_expr(node->callable);
    node->argv = NULL;
    node->argc = 0;
    node->callable = NULL;
    free(node);
}

static void free_cast(struct mtr_cast* node) {
    mtr_free_expr(node->right);
    node->right = NULL;
    free(node);
}

static void free_sub(struct mtr_access* node) {
    mtr_free_expr(node->object);
    mtr_free_expr(node->element);
    node->object = NULL;
    node->element = NULL;
    free(node);
}

void mtr_free_expr(struct mtr_expr* node) {
    switch (node->type)
    {
    case MTR_EXPR_BINARY:   free_binary((struct mtr_binary*) node); return;
    case MTR_EXPR_GROUPING: free_grouping((struct mtr_grouping*) node); return;
    case MTR_EXPR_PRIMARY:  free_primary((struct mtr_primary*) node); return;
    case MTR_EXPR_UNARY:    free_unary((struct mtr_unary*) node); return;
    case MTR_EXPR_LITERAL:  free_literal((struct mtr_literal*) node); return;
    case MTR_EXPR_ARRAY_LITERAL: free_array_lit((struct mtr_array_literal*) node); return;
    case MTR_EXPR_MAP_LITERAL: free_map_lit((struct mtr_map_literal*) node); return;
    case MTR_EXPR_CALL:     free_call((struct mtr_call*) node); return;
    case MTR_EXPR_CAST:     free_cast((struct mtr_cast*) node); return;
    case MTR_EXPR_ACCESS:
    case MTR_EXPR_SUBSCRIPT:
        free_sub((struct mtr_access*) node); return;
    }
}
