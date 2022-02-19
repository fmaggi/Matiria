#ifndef _MTR_TOKEN_H
#define _MTR_TOKEN_H

#include "core/types.h"

enum mtr_token_type {
    // single char
    MTR_TOKEN_PLUS, MTR_TOKEN_MINUS, MTR_TOKEN_STAR, MTR_TOKEN_SLASH, MTR_TOKEN_PERCENT,
    MTR_TOKEN_COMMA, MTR_TOKEN_COLON, MTR_TOKEN_SEMICOLON, MTR_TOKEN_DOT,
    MTR_TOKEN_PAREN_L, MTR_TOKEN_PAREN_R,
    MTR_TOKEN_SQR_L, MTR_TOKEN_SQR_R,
    MTR_TOKEN_CURLY_L, MTR_TOKEN_CURLY_R,
    MTR_TOKEN_BANG,
    MTR_TOKEN_EQUAL,
    MTR_TOKEN_GREATER, MTR_TOKEN_LESS,
    MTR_TOKEN_AND, MTR_TOKEN_OR,

    // double char
    MTR_TOKEN_ARROW,
    MTR_TOKEN_BANG_EQUAL, MTR_TOKEN_EQUAL_EQUAL, MTR_TOKEN_GREATER_EQUAL, MTR_TOKEN_LESS_EQUAL,
    MTR_TOKEN_DOUBLE_SLASH,

    // Literals.
    MTR_TOKEN_STRING, MTR_TOKEN_INT, MTR_TOKEN_FLOAT,

    MTR_TOKEN_STRUCT,
    MTR_TOKEN_IF, MTR_TOKEN_ELSE,
    MTR_TOKEN_TRUE, MTR_TOKEN_FALSE,
    MTR_TOKEN_FN,
    MTR_TOKEN_RETURN,
    MTR_TOKEN_WHILE, MTR_TOKEN_FOR,

    // types
    MTR_TOKEN_U8, MTR_TOKEN_U16, MTR_TOKEN_U32, MTR_TOKEN_U64,
    MTR_TOKEN_I8, MTR_TOKEN_I16, MTR_TOKEN_I32, MTR_TOKEN_I64,
    MTR_TOKEN_F32, MTR_TOKEN_F64,
    MTR_TOKEN_BOOL,

    MTR_TOKEN_IDENTIFIER,

    MTR_TOKEN_COMMENT,
    MTR_TOKEN_EOF,
    MTR_TOKEN_INVALID
};

struct mtr_token {
    enum mtr_token_type type;
    const char* start;
    u32 length;
};

void mtr_print_token(struct mtr_token token);
const char* mtr_token_type_to_str(enum mtr_token_type type);

extern const struct mtr_token invalid_token;

#endif
