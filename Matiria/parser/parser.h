#ifndef MTR_PARSER_H
#define MTR_PARSER_H

#include "scanner/scanner.h"

#include "AST/AST.h"

#include "core/types.h"

struct mtr_parser {
    struct mtr_scanner scanner;
    struct mtr_token token;
    struct mtr_function_decl* current_function;
    struct mtr_type_list* type_list;
    bool had_error;
    bool panic;
};

void mtr_parser_init(struct mtr_parser* parser, const char* source);

struct mtr_ast mtr_parse(struct mtr_parser* parser);

#endif
