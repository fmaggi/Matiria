#include "validator.h"

#include "type.h"
#include "scope.h"

#include "core/report.h"
#include "core/log.h"
#include "debug/dump.h"

#include <string.h>

/*
    TODO: Fix bug where you can assign to functions
*/

static void expr_error(struct mtr_expr* expr, const char* message, const char* const source) {
    switch (expr->type)
    {
    case MTR_EXPR_BINARY: {
        struct mtr_binary* b = (struct mtr_binary*) expr;
        mtr_report_error(b->operator.token, message, source);
        break;
    }
    case MTR_EXPR_CALL: {
        IMPLEMENT
        // struct mtr_call* c = (struct mtr_call*) expr;
        // struct mtr_primary* p = (struct mtr_primary*) c->callable;
        // mtr_report_error(p->symbol.token, message, source);
        break;
    }
    case MTR_EXPR_GROUPING: {
        struct mtr_grouping* g = (struct mtr_grouping*) expr;
        expr_error(g->expression, message, source);
        break;
    }
    case MTR_EXPR_LITERAL: {
        struct mtr_literal* l = (struct mtr_literal*) expr;
        mtr_report_error(l->literal, message, source);
        break;
    }
    case MTR_EXPR_PRIMARY: {
        struct mtr_primary* p = (struct mtr_primary*) expr;
        mtr_report_error(p->symbol.token, message, source);
        break;
    }
    case MTR_EXPR_UNARY: {
        struct mtr_unary* u = (struct mtr_unary*) expr;
        mtr_report_error(u->operator.token, message, source);
        break;
    }

    default:
        break;
    }
}

static struct mtr_type get_operator_type(struct mtr_token op, struct mtr_type lhs, struct mtr_type rhs) {
    struct mtr_type t;

    switch (op.type)
    {
    case MTR_TOKEN_BANG:
    case MTR_TOKEN_OR:
    case MTR_TOKEN_AND:
        t.type = MTR_DATA_BOOL;
        t.obj = NULL;
        break;

    case MTR_TOKEN_PLUS:
    case MTR_TOKEN_MINUS:
    case MTR_TOKEN_STAR:
    case MTR_TOKEN_SLASH:
        t = lhs.type > rhs.type ? lhs : rhs;
        break;
    case MTR_TOKEN_EQUAL:
    case MTR_TOKEN_BANG_EQUAL:
    case MTR_TOKEN_LESS:
    case MTR_TOKEN_LESS_EQUAL:
    case MTR_TOKEN_GREATER:
    case MTR_TOKEN_GREATER_EQUAL:
        t = lhs.type > rhs.type ? lhs : rhs;
        break;
    default:
        t.type = MTR_DATA_INVALID;
        break;
    }
    return t;
}

struct mtr_type mtr_get_data_type(struct mtr_token type) {
    struct mtr_type t = invalid_type;
    switch (type.type)
    {
    case MTR_TOKEN_INT_LITERAL:
    case MTR_TOKEN_INT:
        t.type = MTR_DATA_INT;
        break;

    case MTR_TOKEN_FLOAT_LITERAL:
    case MTR_TOKEN_FLOAT:
        t.type = MTR_DATA_FLOAT;
        break;

    case MTR_TOKEN_BOOL:
    case MTR_TOKEN_TRUE:
    case MTR_TOKEN_FALSE:
        t.type =  MTR_DATA_BOOL;
        break;

    case MTR_TOKEN_IDENTIFIER:
        t.type = MTR_DATA_USER_DEFINED;
        break;

    default:
        MTR_LOG_DEBUG("Invalid data type  %s", mtr_token_type_to_str(type.type));
        break;
    }
    return t;
}

static struct mtr_cast* try_promoting(struct mtr_expr* expr, struct mtr_type type, struct mtr_type to) {
    switch (type.type) {

    case MTR_DATA_INVALID:
    case MTR_DATA_USER_DEFINED:
    case MTR_DATA_ARRAY:
        return NULL;
    case MTR_DATA_BOOL:
    case MTR_DATA_INT:
    case MTR_DATA_FLOAT: {
        if (type.type > to.type) {
            return NULL;
        }
        break;
    }

    }

    struct mtr_cast* cast = malloc(sizeof(*cast));
    cast->expr_.type = MTR_EXPR_CAST;
    cast->to = to;
    cast->right = expr;
    return cast;
}

static struct mtr_type analyze_expr(struct mtr_expr* expr, struct mtr_scope* scope, const char* const source);

static struct mtr_type analyze_binary(struct mtr_binary* expr, struct mtr_scope* scope, const char* const source) {
    const struct mtr_type l = analyze_expr(expr->left, scope, source);
    const struct mtr_type r = analyze_expr(expr->right, scope, source);

    struct mtr_type t = get_operator_type(expr->operator.token, l, r);

    if (t.type == MTR_DATA_INVALID) {
        mtr_report_error(expr->operator.token, "Invalid operation between objects of different types.", source);
        return invalid_type;
    } else if (t.type == MTR_DATA_USER_DEFINED) {
        mtr_report_error(expr->operator.token, "Custom types not yet supported.", source);
        return invalid_type;
    }

    if (!mtr_type_match(l, r)) {
        // if they dont match, t has type either l or r. Depends which one has higher rank.

        // try and mathc the types. Cast if needed
        if (l.type != t.type) {
            struct mtr_cast* cast = try_promoting(expr->left, l, t);
            if (NULL != cast) {
                expr->left = (struct mtr_expr*) cast;
            } else {
                mtr_report_error(expr->operator.token, "Invalid operation between objects of different types.", source);
                return invalid_type;
            }
        } else if (r.type != t.type) {
            struct mtr_cast* cast = try_promoting(expr->right, r, t);
            if (NULL != cast) {
                expr->right = (struct mtr_expr*) cast;
            } else {
                mtr_report_error(expr->operator.token, "Invalid operation between objects of different types.", source);
                return invalid_type;
            }
        }
    }

    expr->operator.type = t;
    return  expr->operator.type;
}

static struct mtr_type analyze_primary(struct mtr_primary* expr, struct mtr_scope* scope, const char* const source) {
    struct mtr_symbol_entry* e = mtr_scope_find(scope, expr->symbol.token);
    if (NULL == e) {
        mtr_report_error(expr->symbol.token, "Undeclared variable.", source);
        return invalid_type;
    }

    struct mtr_symbol s = e->symbol;
    expr->symbol.index = s.index;
    expr->symbol.type = s.type;
    return s.type;
}

static struct mtr_type analyze_literal(struct mtr_literal* literal, struct mtr_scope* scope, const char* const source) {
    struct mtr_type t = mtr_get_data_type(literal->literal);
    return t;
}

static struct mtr_type analyze_call(struct mtr_call* call, struct mtr_scope* scope, const char* const source) {
    // if (call->callable->type != MTR_EXPR_PRIMARY) {
    //     expr_error(call->callable, "Expression is not callable.", source);
    //     return invalid_type;
    // }

    // struct mtr_primary* p = (struct mtr_primary*) call->callable;

    struct mtr_symbol_entry* e = mtr_scope_find(scope, call->callable.token);
    if (NULL == e) {
        mtr_report_error(call->callable.token, "Call to undefined function.", source);
        return invalid_type;
    }

    struct mtr_symbol s = e->symbol;
    struct mtr_function_decl* f = (struct mtr_function_decl*) e->parent;
    if (f->argc == call->argc) {
        for (u8 i = 0 ; i < call->argc; ++i) {
            struct mtr_expr* a = call->argv[i];
            struct mtr_type ta = analyze_expr(a, scope, source);

            struct mtr_variable p = f->argv[i];
            if (!mtr_type_match(ta, p.symbol.type)) {
                expr_error(a, "Wrong type of argument.", source);
                s.type.type = MTR_DATA_INVALID;
            }
        }

    } else if (f->argc > call->argc) {
        mtr_report_error(call->callable.token, "Expected more arguments.", source);
    } else {
        mtr_report_error(call->callable.token, "Too many arguments.", source);
    }

    call->callable.index = s.index;
    call->callable.type = s.type;
    return s.type;
}

static struct mtr_type analyze_subscript(struct mtr_subscript* expr, struct mtr_scope* scope, const char* const source) {
    if (expr->object->type != MTR_EXPR_PRIMARY) {
        expr_error(expr->object, "Expression is not subscriptable.", source);
        return invalid_type;
    }

    struct mtr_primary* p = (struct mtr_primary*) expr->object;
    struct mtr_symbol_entry* e = mtr_scope_find(scope, p->symbol.token);
    if (NULL == e) {
        mtr_report_error(p->symbol.token, "Undefined variable.", source);
        return invalid_type;
    }

    struct mtr_symbol s = e->symbol;
    if (s.type.type != MTR_DATA_ARRAY) {
        mtr_report_error(p->symbol.token, "Variable is not array.", source);
        return invalid_type;
    }

    struct mtr_type index_type = analyze_expr(expr->index, scope, source);
    if (index_type.type != MTR_DATA_INT) {
        expr_error(expr->index, "Index has to be integral expression.", source);
        return invalid_type;
    }

    p->symbol.type = s.type;
    p->symbol.index = s.index;

    return mtr_get_underlying_type(s.type);
}

static struct mtr_type analyze_unary(struct mtr_unary* expr, struct mtr_scope* scope, const char* const source) {
    const struct mtr_type r = analyze_expr(expr->right, scope, source);
    struct mtr_type dummy = invalid_type;
    expr->operator.type = get_operator_type(expr->operator.token, r, dummy);

    return  expr->operator.type;
}

static struct mtr_type analyze_expr(struct mtr_expr* expr, struct mtr_scope* scope, const char* const source) {
    switch (expr->type)
    {
    case MTR_EXPR_BINARY:   return analyze_binary((struct mtr_binary*) expr, scope, source);
    case MTR_EXPR_GROUPING: return analyze_expr(((struct mtr_grouping*) expr)->expression, scope, source);
    case MTR_EXPR_UNARY:    return analyze_unary(((struct mtr_unary*) expr), scope, source);
    case MTR_EXPR_PRIMARY:  return analyze_primary((struct mtr_primary*) expr, scope, source);
    case MTR_EXPR_LITERAL:  return analyze_literal((struct mtr_literal*) expr, scope, source);
    case MTR_EXPR_CALL:     return analyze_call((struct mtr_call*) expr, scope, source);
    case MTR_EXPR_SUBSCRIPT: return analyze_subscript((struct mtr_subscript*) expr, scope, source);
    default:
        break;
    }
    MTR_ASSERT(false, "Invalid stmt type.");
    return invalid_type;
}

static bool load_fn(struct mtr_function_decl* stmt, struct mtr_scope* scope, const char* const source) {
    const struct mtr_symbol_entry* e = mtr_scope_find(scope, stmt->symbol.token);
    if (NULL != e) {
        mtr_report_error(stmt->symbol.token, "Redefinition of name.", source);
        mtr_report_message(e->symbol.token, "Previuosly defined here.", source);
        return false;
    }

    stmt->symbol.index = scope->current++;
    mtr_scope_add(scope, stmt->symbol, (struct mtr_stmt*) stmt);
    return true;
}

static bool load_var(struct mtr_variable* stmt, struct mtr_scope* scope, const char* const source) {
    const struct mtr_symbol_entry* e = mtr_scope_find(scope, stmt->symbol.token);
    if (NULL != e) {
        mtr_report_error(stmt->symbol.token, "Redefinition of name.", source);
        mtr_report_message(e->symbol.token, "Previuosly defined here.", source);
        return false;
    }

    stmt->symbol.index = scope->current++;
    mtr_scope_add(scope, stmt->symbol, (struct mtr_stmt*) stmt);
    return true;
}

static bool analyze(struct mtr_stmt* stmt, struct mtr_scope* parent, const char* const source);

static bool analyze_block(struct mtr_block* block, struct mtr_scope* parent, const char* const source) {
    bool all_ok = true;

    struct mtr_scope scope = mtr_new_scope(parent);
    size_t current = scope.current;

    for (size_t i = 0; i < block->size; ++i) {
        struct mtr_stmt* s = block->statements[i];
        bool s_ok = analyze(s, &scope, source);
        all_ok = s_ok && all_ok;
    }

    block->var_count = (u16) (scope.current - current);
    mtr_delete_scope(&scope);
    return all_ok;
}

static bool analyze_fn(struct mtr_function_decl* stmt, struct mtr_scope* parent, const char* const source) {
    if (NULL == stmt->body) {
        // Function is extern
        return true;
    }

    bool all_ok = true;

    struct mtr_scope scope = mtr_new_scope(parent);

    for (size_t i = 0; i < stmt->argc; ++i) {
        struct mtr_variable* arg = stmt->argv + i;
        all_ok = load_var(arg, &scope, source) && all_ok;
    }

    all_ok = analyze_block(stmt->body, &scope, source) && all_ok;
    mtr_delete_scope(&scope);

    return all_ok;
}

static bool analyze_assignment(struct mtr_assignment* stmt, struct mtr_scope* parent, const char* const source) {
    const struct mtr_symbol_entry* e = mtr_scope_find(parent, stmt->variable.token);
    bool var_ok = true;
    if (NULL == e) {
        mtr_report_error(stmt->variable.token, "Undeclared variable.", source);
        var_ok = false;
    }
    struct mtr_symbol s = e->symbol;
    stmt->variable.index = s.index;
    stmt->variable.type = s.type;
    const struct mtr_type expr = analyze_expr(stmt->expression, parent, source);

    bool expr_ok = mtr_type_match(expr, s.type);
    if (!expr_ok) {
        // try and mathc the types. Cast if needed
        struct mtr_cast* cast = try_promoting(stmt->expression, expr, stmt->variable.type);
        if (NULL != cast) {
            stmt->expression = (struct mtr_expr*) cast;
            expr_ok = true;
        } else {
            mtr_report_error(stmt->variable.token, "Invalid assignement to variable of different type", source);
        }
    }

    return var_ok && expr_ok;
}

static bool analyze_variable(struct mtr_variable* decl, struct mtr_scope* parent, const char* const source) {
    bool expr = true;
    if (decl->value) {
        const struct mtr_type type = analyze_expr(decl->value, parent, source);
        if (decl->symbol.type.type == MTR_DATA_INVALID) { // this means it was a 'let' var decl
            decl->symbol.type = type;
        } else if (!mtr_type_match(decl->symbol.type, type)) {
            // try and mathc the types. Cast if needed
            struct mtr_cast* cast = try_promoting(decl->value, type, decl->symbol.type);
            if (NULL != cast) {
                decl->value = (struct mtr_expr*) cast;
            } else {
                mtr_report_error(decl->symbol.token, "Invalid assignement to variable of different type", source);
                expr = false;
            }
        }
    }

    bool loaded = load_var(decl, parent, source);
    return loaded && expr;
}

static bool analyze_if(struct mtr_if* stmt, struct mtr_scope* parent, const char* const source) {
    struct mtr_type expr_type = analyze_expr(stmt->condition, parent, source);
    bool condition_ok = expr_type.type == MTR_DATA_FLOAT || expr_type.type == MTR_DATA_INT || expr_type.type == MTR_DATA_BOOL;
    if (!condition_ok) {
        expr_error(stmt->condition, "Expression doesn't return Bool.", source);
    }

    bool then_ok = analyze(stmt->then, parent, source);

    bool e_ok = true;
    if (stmt->otherwise) {
        e_ok = analyze(stmt->otherwise, parent, source);
    }

    return condition_ok && then_ok && e_ok;
}

static bool analyze_while(struct mtr_while* stmt, struct mtr_scope* parent, const char* const source) {
    struct mtr_type expr_type = analyze_expr(stmt->condition, parent, source);
    bool condition_ok = expr_type.type == MTR_DATA_FLOAT || expr_type.type == MTR_DATA_INT || expr_type.type == MTR_DATA_BOOL;
    if (!condition_ok) {
        expr_error(stmt->condition, "Expression doesn't return Bool.", source);
    }

    bool body_ok = analyze(stmt->body, parent, source);

    return condition_ok && body_ok;
}

static bool analyze_return(struct mtr_return* stmt, struct mtr_scope* parent, const char* const source) {
    bool ok = mtr_type_match(analyze_expr(stmt->expr, parent, source), stmt->from.type);
    if (!ok) {
        expr_error(stmt->expr, "Incompatible return type.", source);
        mtr_report_message(stmt->from.token, "As declared here.", source);
    }
    return ok;
}

static bool analyze(struct mtr_stmt* stmt, struct mtr_scope* scope, const char* const source) {
    switch (stmt->type)
    {
    case MTR_STMT_BLOCK:      return analyze_block((struct mtr_block*) stmt, scope, source);
    case MTR_STMT_ASSIGNMENT: return analyze_assignment((struct mtr_assignment*) stmt, scope, source);
    case MTR_STMT_FN:         return analyze_fn((struct mtr_function_decl*) stmt, scope, source);
    case MTR_STMT_VAR:        return analyze_variable((struct mtr_variable*) stmt, scope, source);
    case MTR_STMT_IF:         return analyze_if((struct mtr_if*) stmt, scope, source);
    case MTR_STMT_WHILE:      return analyze_while((struct mtr_while*) stmt, scope, source);
    case MTR_STMT_RETURN:     return analyze_return((struct mtr_return*) stmt, scope, source);
    default:
        break;
    }
    MTR_ASSERT(false, "Invalid stmt type.");
    return false;
}

static bool global_analysis(struct mtr_stmt* stmt, struct mtr_scope* scope, const char* const source) {
    switch (stmt->type)
    {
    case MTR_STMT_FN: return analyze_fn((struct mtr_function_decl*) stmt, scope, source);
    default:
        break;
    }
    MTR_ASSERT(false, "Invalid stmt type.");
    return false;
}

static bool load_global(struct mtr_stmt* stmt, struct mtr_scope* scope, const char* const source) {
    switch (stmt->type)
    {
    case MTR_STMT_FN:     return load_fn((struct mtr_function_decl*) stmt, scope, source);
    default:
        break;
    }
    MTR_ASSERT(false, "Invalid stmt type.");
    return false;
}

bool mtr_validate(struct mtr_ast* ast, const char* const source) {
    bool all_ok = true;

    struct mtr_scope global = mtr_new_scope(NULL);

    struct mtr_block* block = (struct mtr_block*) ast->head;

    for (size_t i = 0; i < block->size; ++i) {
        struct mtr_stmt* s = block->statements[i];
        all_ok = load_global(s, &global, source);
    }

    global.current = 0;

    for (size_t i = 0; i < block->size; ++i) {
        struct mtr_stmt* s = block->statements[i];
        all_ok = global_analysis(s, &global, source) && all_ok;
    }

    mtr_delete_scope(&global);

    return all_ok;
}
