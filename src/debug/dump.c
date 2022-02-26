#ifndef NDEBUG

#include "dump.h"

#include "core/log.h"

#include <string.h>

void mtr_dump_token(struct mtr_token token) {
    const char* type = mtr_token_type_to_str(token.type);

    if (token.type == MTR_TOKEN_IDENTIFIER || token.type == MTR_TOKEN_STRING_LITERAL || token.type == MTR_TOKEN_INT_LITERAL ||token.type == MTR_TOKEN_FLOAT_LITERAL || token.type == MTR_TOKEN_INVALID) {
        MTR_LOG_DEBUG("Token: %s, (%.*s)", type, (u32)token.length, token.start);
    } else {
        MTR_LOG_DEBUG("Token: %s", type);
    }
}

static void dump_expr(struct mtr_expr* expr, u32 offset) {
    if (offset > 0 && offset < 256) {
        char buf[256];
        memset(buf, ' ', 256);
        MTR_PRINT_DEBUG("%.*s", offset, buf);
    }

    switch (expr->type)
    {
    case MTR_EXPR_PRIMARY: {
        struct mtr_primary* p = (struct mtr_primary*) expr;
        MTR_PRINT_DEBUG("%.*s", (u32)p->symbol.token.length, p->symbol.token.start);
        break;
    }
    case MTR_EXPR_UNARY: {
        struct mtr_unary* u = (struct mtr_unary*) expr;
        MTR_PRINT_DEBUG("%s", mtr_token_type_to_str(u->operator.type));
        dump_expr(u->right, 0);
        break;
    }
    case MTR_EXPR_GROUPING: {
        struct mtr_grouping* g = (struct mtr_grouping*) expr;
        MTR_PRINT_DEBUG("(");
        dump_expr(g->expression, 0);
        MTR_PRINT_DEBUG(")");
        break;
    }
    case MTR_EXPR_BINARY: {
        struct mtr_binary* b = (struct mtr_binary*) expr;
        MTR_PRINT_DEBUG("(%s ", mtr_token_type_to_str(b->operator.type));
        dump_expr(b->left, 0);
        MTR_PRINT_DEBUG(" ");
        dump_expr(b->right, 0);
        MTR_PRINT_DEBUG(")");
        break;
    }
    }
}

void mtr_dump_expr(struct mtr_expr* expr) {
    dump_expr(expr, 0);
}

static void dump_stmt(struct mtr_stmt* stmt, u32 offset);

static void dump_block(struct mtr_block* block, u32 offset) {
    for (size_t i = 0; i < block->statements.size; ++i) {
        struct mtr_stmt* s = block->statements.statements + i;
        dump_stmt(s, offset + 1);
    }
}

static void dump_fn(struct mtr_function* decl, u32 offset) {
    MTR_PRINT_DEBUG("Function: %.*s(", (u32)decl->symbol.token.length, decl->symbol.token.start);
    if (decl->argc > 0) {
        for (u32 i = 0; i < decl->argc - 1; ++i) {
            struct mtr_variable param = decl->argv[i];
            MTR_PRINT_DEBUG("%.*s, ", (u32)param.symbol.token.length, param.symbol.token.start);
        }

        struct mtr_variable param = decl->argv[decl->argc-1];
        MTR_PRINT_DEBUG("%.*s", (u32)param.symbol.token.length, param.symbol.token.start);
    }

    MTR_PRINT_DEBUG(") -> %s {\n", mtr_data_type_to_str(decl->symbol.type));
    dump_block(&decl->body, offset + 1);
    MTR_PRINT_DEBUG("}\n");
}

static void dump_var(struct mtr_variable* decl, u32 offset) {
    MTR_PRINT_DEBUG("%s %.*s", mtr_data_type_to_str(decl->symbol.type), (u32)decl->symbol.token.length,decl->symbol.token.start);
    if (decl->value) {
        MTR_PRINT_DEBUG(" := ");
        dump_expr(decl->value, 0);
    }
    MTR_PRINT_DEBUG(";\n");
}

static void dump_if(struct mtr_if* stmt, u32 offset) {
    MTR_PRINT_DEBUG("if: ");
    dump_expr(stmt->condition, 0);
    MTR_PRINT_DEBUG("\n");
    dump_block(&stmt->then, offset + 1);
    MTR_PRINT_DEBUG("else: \n");
    dump_block(&stmt->else_b, offset + 1);
    MTR_PRINT_DEBUG("\n");
}

static void dump_while(struct mtr_while* stmt, u32 offset) {
    MTR_PRINT_DEBUG("loop: ");
    dump_expr(stmt->condition, 0);
    MTR_PRINT_DEBUG("\n");
    dump_block(&stmt->body, offset + 1);
    MTR_PRINT_DEBUG("\n");
}

static void dump_assignment(struct mtr_assignment* stmt, u32 offset) {
    MTR_PRINT_DEBUG("%.*s", (u32)stmt->variable.token.length, stmt->variable.token.start);
    MTR_PRINT_DEBUG(" := ");
    dump_expr(stmt->expression, 0);
    MTR_PRINT_DEBUG(";\n");
}

static void dump_stmt(struct mtr_stmt* stmt, u32 offset) {
    if (offset > 0 && offset < 256) {
        char buf[256];
        memset(buf, ' ', 256);
        MTR_PRINT_DEBUG("%.*s", offset, buf);
    }

    switch (stmt->type)
    {
    case MTR_STMT_FN: return dump_fn((struct mtr_function*) stmt, offset);
    case MTR_STMT_BLOCK: return dump_block((struct mtr_block*) stmt, offset);
    case MTR_STMT_VAR: return dump_var((struct mtr_variable*) stmt, offset);
    case MTR_STMT_IF: return dump_if((struct mtr_if*) stmt, offset);
    case MTR_STMT_WHILE: return dump_while((struct mtr_while*) stmt, offset);
    case MTR_STMT_ASSIGNMENT: return dump_assignment((struct mtr_assignment*) stmt, offset);
    default:
        break;
    }
}

void mtr_dump_stmt(struct mtr_stmt* stmt) {
    dump_stmt(stmt, 0);
}

const char* mtr_token_type_to_str(enum mtr_token_type type) {
    switch (type)
    {
    case MTR_TOKEN_PLUS:          return "+";
    case MTR_TOKEN_MINUS:         return "-";
    case MTR_TOKEN_STAR:          return "*";
    case MTR_TOKEN_SLASH:         return "/";
    case MTR_TOKEN_PERCENT:       return "%";
    case MTR_TOKEN_COMMA:         return ",";
    case MTR_TOKEN_COLON:         return ":";
    case MTR_TOKEN_SEMICOLON:     return ";";
    case MTR_TOKEN_DOT:           return ".";
    case MTR_TOKEN_PAREN_L:       return "(";
    case MTR_TOKEN_PAREN_R:       return ")";
    case MTR_TOKEN_SQR_L:         return "[";
    case MTR_TOKEN_SQR_R:         return "]";
    case MTR_TOKEN_CURLY_L:       return "{";
    case MTR_TOKEN_CURLY_R:       return "}";
    case MTR_TOKEN_BANG:          return "!";
    case MTR_TOKEN_ASSIGN:        return ":=";
    case MTR_TOKEN_GREATER:       return ">";
    case MTR_TOKEN_LESS:          return "<";
    case MTR_TOKEN_ARROW:         return "->";
    case MTR_TOKEN_BANG_EQUAL:    return "!=";
    case MTR_TOKEN_EQUAL:         return "=";
    case MTR_TOKEN_GREATER_EQUAL: return ">=";
    case MTR_TOKEN_LESS_EQUAL:    return "<=";
    case MTR_TOKEN_DOUBLE_SLASH:  return "//";
    case MTR_TOKEN_STRING_LITERAL:return "STRING";
    case MTR_TOKEN_INT_LITERAL:   return "INT";
    case MTR_TOKEN_FLOAT_LITERAL: return "FLOAT";
    case MTR_TOKEN_AND:           return "&&";
    case MTR_TOKEN_OR:            return "||";
    case MTR_TOKEN_STRUCT:        return "struct";
    case MTR_TOKEN_IF:            return "if";
    case MTR_TOKEN_ELSE:          return "else";
    case MTR_TOKEN_TRUE:          return "true";
    case MTR_TOKEN_FALSE:         return "false";
    case MTR_TOKEN_FN:            return "fn";
    case MTR_TOKEN_RETURN:        return "return";
    case MTR_TOKEN_WHILE:         return "while";
    case MTR_TOKEN_FOR:           return "for";
    case MTR_TOKEN_INT:           return "Int";
    case MTR_TOKEN_FLOAT:         return "Float";
    case MTR_TOKEN_BOOL:          return "Bool";
    case MTR_TOKEN_IDENTIFIER:    return "IDENTIFIER";
    case MTR_TOKEN_COMMENT:       return "comment";
    case MTR_TOKEN_EOF:           return "EOF";
    case MTR_TOKEN_INVALID:       return "invalid";
    }
    return "invalid";
}

const char* mtr_data_type_to_str(struct mtr_data_type type) {
    switch (type.type)
    {
    case MTR_DATA_BOOL:    return "Bool";
    case MTR_DATA_FLOAT:   return "Float";
    case MTR_DATA_INT:     return "Int";
    case MTR_DATA_INVALID: return "Invalid";
    case MTR_DATA_USER_DEFINED: break;
    }

    static char buf[256];
    memset(buf, 0, 256);
    if (type.length < 256) {
        memcpy(buf, type.user_struct, type.length);
    }
    return buf;
}

#endif