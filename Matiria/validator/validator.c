#include "validator.h"

#include "AST/AST.h"
#include "AST/symbol.h"
#include "symbolTable.h"

#include "core/report.h"
#include "core/log.h"
#include "debug/dump.h"

#include <stdlib.h>
#include <string.h>

struct validator {
    struct mtr_symbol_table symbols;
    size_t count;
    struct validator* enclosing;
    struct mtr_closure_decl* closure;
    struct mtr_type_list* type_list;
    const char* source;
};

static void init_validator(struct validator* validator, struct validator* enclosing) {
    validator->enclosing = enclosing;
    validator->closure = enclosing->closure;
    mtr_init_symbol_table(&validator->symbols);
    validator->source = enclosing->source;
    validator->type_list = enclosing->type_list;

    bool should_be_zero = enclosing == NULL || enclosing->enclosing == NULL;
    validator->count = should_be_zero ? 0 : enclosing->count;
}

static void delete_validator(struct validator* validator) {
    mtr_delete_symbol_table(&validator->symbols);
}

static struct mtr_symbol* find_symbol(const struct validator* validator, struct mtr_token token) {
    struct mtr_symbol* s = mtr_symbol_table_get(&validator->symbols, token.start, token.length);
    while (NULL == s && NULL != validator->enclosing) {
        validator = validator->enclosing;
        s = mtr_symbol_table_get(&validator->symbols, token.start, token.length);
    }
    return s;
}

static size_t add_symbol(struct validator* validator, struct mtr_symbol symbol) {
    struct mtr_symbol* s = find_symbol(validator, symbol.token);
    if (NULL != s) {
        return -1;
    }

    symbol.index = validator->count++;
    symbol.is_global = validator->enclosing == NULL;
    symbol.upvalue = false;
    mtr_symbol_table_insert(&validator->symbols, symbol.token.start, symbol.token.length, symbol);
    return symbol.index;
}

static size_t resolve_local(struct validator* validator, struct mtr_symbol symbol) {
    struct mtr_token token = symbol.token;
    struct mtr_symbol* s = mtr_symbol_table_get(&validator->symbols, token.start, token.length);
    return s ? s->index : -1;
}

static size_t add_upvalue(struct validator* validator, struct mtr_symbol symbol, bool local) {
    struct mtr_closure_decl* closure = validator->closure;

    if (closure->count >= UINT16_MAX) {
        return -1;
    }

    if (closure->upvalues == NULL) {
        closure->upvalues = malloc(sizeof(struct mtr_upvalue_symbol) * 8);
        closure->capacity = 8;
        closure->count = 0;
    }

    for (u8 i = 0; i < closure->count; ++i) {
        if (mtr_token_compare(symbol.token, closure->upvalues[i].token)) {
            return i;
        }
    }

    if (closure->count == closure->capacity) {
        closure->capacity *= 2;
        closure->upvalues = realloc(closure->upvalues, sizeof(struct mtr_upvalue_symbol) * closure->capacity);
    }

    u16 index = closure->count++;

    closure->upvalues[index].token = symbol.token;
    closure->upvalues[index].index = symbol.index;
    closure->upvalues[index].local = local;
    return index;
}

static size_t resolve_upvalue(struct validator* validator, struct mtr_symbol symbol) {
    if (validator->enclosing == NULL) {
        return -1;
    }

    size_t i = resolve_local(validator->enclosing, symbol);
    if (i != (size_t) -1) {
        return add_upvalue(validator, symbol, true);
    }

    i = resolve_upvalue(validator->enclosing, symbol);
    if (i != (size_t) -1) {
        symbol.index = i;
        return add_upvalue(validator, symbol, false);
    }

    return -1;
}

#define TYPE_CHECK(t) \
    do {              \
        if (t == NULL || t->type == MTR_DATA_INVALID) return NULL; \
    } while(0)

static bool is_literal(struct mtr_expr* expr) {
    return expr->type == MTR_EXPR_ARRAY_LITERAL || expr->type == MTR_EXPR_MAP_LITERAL || expr->type == MTR_EXPR_LITERAL;
}

// static bool is_function(struct mtr_type* t) {
//     return t.type == MTR_DATA_FN;
// }

static bool check_assignemnt(const struct mtr_type* assign_to, const struct mtr_type* what) {
    if (assign_to == what) {
        return true;
    }

    if (assign_to->type == MTR_DATA_ANY) {
        return true;
    }

    if (!mtr_type_match(assign_to, what)) {
        if (assign_to && assign_to->type == MTR_DATA_UNION) {
            struct mtr_union_type* u = (struct mtr_union_type*) assign_to;
            for (u8 i = 0; i < u->argc; ++i) {
                if (mtr_type_match(u->types[i], what)) {
                    return true;
                }
            }
        }

        return false;
    }
    return true;
}

static void expr_error(struct mtr_expr* expr, const char* message, const char* source) {
    switch (expr->type)
    {
    case MTR_EXPR_BINARY: {
        struct mtr_binary* b = (struct mtr_binary*) expr;
        mtr_report_error(b->operator.token, message, source);
        break;
    }
    case MTR_EXPR_CALL: {
        struct mtr_call* c = (struct mtr_call*) expr;
        expr_error(c->callable, message, source);
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

    case MTR_EXPR_ACCESS:
    case MTR_EXPR_SUBSCRIPT: {
        struct mtr_access* s = (struct mtr_access*) expr;
        expr_error(s->object, message, source);
        break;
    }

    default:
        break;

    }
}

static struct mtr_type* get_operator_type(struct mtr_type_list* list, struct mtr_token op, const struct mtr_type* lhs, const struct mtr_type* rhs) {
    struct mtr_type t;

    switch (op.type)
    {
    case MTR_TOKEN_BANG:
    case MTR_TOKEN_OR:
    case MTR_TOKEN_AND:
        t.type = MTR_DATA_BOOL;
        break;

    case MTR_TOKEN_PLUS:
    case MTR_TOKEN_MINUS:
    case MTR_TOKEN_STAR:
    case MTR_TOKEN_SLASH:
        t = *(lhs->type > rhs->type ? lhs : rhs);
        break;
    case MTR_TOKEN_EQUAL:
    case MTR_TOKEN_BANG_EQUAL:
    case MTR_TOKEN_LESS:
    case MTR_TOKEN_LESS_EQUAL:
    case MTR_TOKEN_GREATER:
    case MTR_TOKEN_GREATER_EQUAL:
        t = *(lhs->type > rhs->type ? lhs : rhs);
        break;
    default:
        t.type = MTR_DATA_INVALID;
        break;
    }
    return mtr_type_list_exists(list, t);
}

static struct mtr_type* analyze_expr(struct mtr_expr* expr, struct validator* validator);

static struct mtr_type* analyze_binary(struct mtr_binary* expr, struct validator* validator) {
    const struct mtr_type* l = analyze_expr(expr->left, validator);
    const struct mtr_type* r = analyze_expr(expr->right, validator);

    TYPE_CHECK(l);
    TYPE_CHECK(r);

    struct mtr_type* t = get_operator_type(validator->type_list, expr->operator.token, l, r);

    if (!t || t->type == MTR_DATA_INVALID) {
        mtr_report_error(expr->operator.token, "Invalid operation between objects of different types.", validator->source);
        return NULL;
    }

    if (l != r) {
        mtr_report_error(expr->operator.token, "Invalid operation between objects of different types.", validator->source);
        return NULL;
    }

    expr->operator.type = t;
    return  expr->operator.type;
}

static struct mtr_type* analyze_primary(struct mtr_primary* expr, struct validator* validator) {
    struct mtr_symbol* symbol = find_symbol(validator, expr->symbol.token);

    if (symbol == NULL) {
        mtr_report_error(expr->symbol.token, "Undeclared variable.", validator->source);
        return NULL;
    }

    expr->symbol.type = symbol->type;
    expr->symbol.index = symbol->index;
    expr->symbol.flags = symbol->flags;

    bool check = validator->closure && !symbol->is_global;

    // because we are here we know the symbol exists
    // probably could optimize this
    if (check) {
        size_t i = resolve_local(validator, expr->symbol);
        if (i == (size_t) -1) {
            i = resolve_upvalue(validator, expr->symbol);
            expr->symbol.upvalue = true;
        }
        expr->symbol.index = i;
    }

    return symbol->type;
}

static struct mtr_type* analyze_literal(struct mtr_literal* literal, struct validator* validator) {
    return mtr_type_list_register_from_token(validator->type_list, literal->literal);
}

static struct mtr_type* analyze_array_literal(struct mtr_array_literal* array, struct validator* validator) {
    struct mtr_expr* first = array->expressions[0];
    struct mtr_type* array_type = analyze_expr(first, validator);
    TYPE_CHECK(array_type);

    for (u8 i = 1; i < array->count; ++i) {
        struct mtr_expr* e = array->expressions[i];
        struct mtr_type* t = analyze_expr(e, validator);
        if (array_type != t) {
            expr_error(e, "Array literal must contain expressions of the same type", validator->source);
            return NULL;
        }
    }

    return mtr_type_list_register_array(validator->type_list, array_type);
}

static struct mtr_type* analyze_map_literal(struct mtr_map_literal* map, struct validator* validator) {
    struct mtr_map_entry first = map->entries[0];
    struct mtr_type* key_type = analyze_expr(first.key, validator);
    struct mtr_type* val_type = analyze_expr(first.value, validator);

    TYPE_CHECK(key_type);
    TYPE_CHECK(val_type);

    for (u8 i = 1; i < map->count; ++i) {
        struct mtr_map_entry e = map->entries[i];
        struct mtr_type* k_t = analyze_expr(e.key, validator);
        struct mtr_type* v_t = analyze_expr(e.value, validator);
        if (key_type != k_t || val_type != v_t) {
            expr_error(e.key, "Map literal must contain expressions of the same type", validator->source);
            return NULL;
        }
    }

    return mtr_type_list_register_map(validator->type_list, key_type, val_type);
}

static bool check_params(struct mtr_function_type* f, struct mtr_call* call, struct validator* validator) {
    for (u8 i = 0 ; i < call->argc; ++i) {
        struct mtr_expr* a = call->argv[i];
        struct mtr_type* from = analyze_expr(a, validator);
        if (!from) {
            return false;
        }

        struct mtr_type* to = f->argv[i];
        bool match = check_assignemnt(to, from);
        if (!match) {
            expr_error(a, "Wrong type of argument.", validator->source);
            return false;
        }
    }
    return true;
}

// static struct mtr_type* curry_call(struct mtr_call* call, struct mtr_type* type, struct validator* validator) {
//     struct mtr_function_type* f = type.obj;
//     bool match = check_params(f, call, validator);
//     if (!match) {
//         return invalid_type;
//     }

//     return mtr_new_function_type(f->return_, f->argc - call->argc, f->argv + call->argc);
// }

static struct mtr_type* function_call(struct mtr_call* call, struct mtr_type* type, struct validator* validator) {
    struct mtr_function_type* fc =  (struct mtr_function_type*) type;
    if (fc->argc == call->argc && check_params(fc, call, validator)) {
        return fc->return_;
    } else if (fc->argc > call->argc) {
        expr_error(call->callable, "Expected more arguments.", validator->source);
    } else if (fc->argc < call->argc) {
        expr_error(call->callable, "Too many arguments.", validator->source);
    }

    return NULL;
}

static struct mtr_type* analyze_call(struct mtr_call* call, struct validator* validator) {
    struct mtr_type* type = analyze_expr(call->callable, validator);
    TYPE_CHECK(type);

    if (type->type == MTR_DATA_FN) {
        return function_call(call, type, validator);
    }

    expr_error(call->callable, "Expression is not callable.", validator->source);
    return NULL;
}

static struct mtr_type* analyze_subscript(struct mtr_access* expr, struct validator* validator) {
    struct mtr_type* type = analyze_expr(expr->object, validator);
    struct mtr_type* index_type = analyze_expr(expr->element, validator);
    TYPE_CHECK(type);
    TYPE_CHECK(index_type);

    switch (type->type) {

    case MTR_DATA_ARRAY: {
        if (index_type->type != MTR_DATA_INT) {
            expr_error(expr->element, "Index has to be integral expression.", validator->source);
            return NULL;
        }
        break;
    }

    case MTR_DATA_MAP: {
        struct mtr_map_type* m = (struct mtr_map_type*) type;
        if (index_type != m->key) {
            expr_error(expr->element, "Index doesn't match key type.", validator->source);
            return NULL;
        }
        break;
    }

    default:
        expr_error(expr->object, "Expression is not subscriptable.", validator->source);
        return NULL;
    }

    return mtr_get_underlying_type(type);;
}

static struct mtr_type* analyze_unary(struct mtr_unary* expr, struct validator* validator) {
    const struct mtr_type* r = analyze_expr(expr->right, validator);
    struct mtr_type* dummy = NULL;
    expr->operator.type = get_operator_type(validator->type_list, expr->operator.token, r, dummy);

    return  expr->operator.type;
}

static struct mtr_type* analyze_access(struct mtr_access* expr, struct validator* validator) {
    const struct mtr_type* right_t = analyze_expr(expr->object, validator);
    TYPE_CHECK(right_t);

    if (right_t->type != MTR_DATA_STRUCT) {
        expr_error(expr->object, "Expression is not accessible.", validator->source);
        return NULL;
    }

    if (expr->element->type != MTR_EXPR_PRIMARY) {
        expr_error(expr->element, "Expression cannot be used as access expression.", validator->source);
        return NULL;
    }

    struct mtr_primary* p = (struct mtr_primary*) expr->element;
    const struct mtr_struct_type* st = (struct mtr_struct_type*) right_t;
    for (u8 i = 0; i < st->argc; ++i) {
        bool match = mtr_token_compare(st->members[i]->token, p->symbol.token);
        if (match) {
            p->symbol.index = i;
            return st->members[i]->type;
        }
    }
    expr_error(expr->element, "No member.", validator->source);
    return NULL;
}

static struct mtr_type* analyze_expr(struct mtr_expr* expr, struct validator* validator) {
    switch (expr->type)
    {
    case MTR_EXPR_BINARY:   return analyze_binary((struct mtr_binary*) expr, validator);
    case MTR_EXPR_GROUPING: return analyze_expr(((struct mtr_grouping*) expr)->expression, validator);
    case MTR_EXPR_UNARY:    return analyze_unary(((struct mtr_unary*) expr), validator);
    case MTR_EXPR_PRIMARY:  return analyze_primary((struct mtr_primary*) expr, validator);
    case MTR_EXPR_LITERAL:  return analyze_literal((struct mtr_literal*) expr, validator);
    case MTR_EXPR_ARRAY_LITERAL: return analyze_array_literal((struct mtr_array_literal*) expr, validator);
    case MTR_EXPR_MAP_LITERAL: return analyze_map_literal((struct mtr_map_literal*) expr, validator);
    case MTR_EXPR_CALL:     return analyze_call((struct mtr_call*) expr, validator);
    case MTR_EXPR_SUBSCRIPT: return analyze_subscript((struct mtr_access*) expr, validator);
    case MTR_EXPR_ACCESS: return analyze_access((struct mtr_access*) expr, validator);
    case MTR_EXPR_CAST:     IMPLEMENT return NULL;
    }
    MTR_ASSERT(false, "Invalid stmt type.");
    return NULL;
}

static bool load_fn(struct mtr_function_decl* stmt, struct validator* validator) {
    size_t i = add_symbol(validator, stmt->symbol);
    if (i == (size_t) -1) {
        mtr_report_error(stmt->symbol.token, "Redefinition of name.", validator->source);

        struct mtr_symbol* s = find_symbol(validator, stmt->symbol.token);
        mtr_report_message(s->token, "Previuosly defined here.", validator->source);
        return false;
    }

    stmt->symbol.index = i;
    return true;
}

static bool load_var(struct mtr_variable* stmt, struct validator* validator) {
    if (stmt->symbol.type->type == MTR_DATA_ANY) {
        mtr_report_error(stmt->symbol.token, "'Any' expressions are only allowed as parameters to native functions.", validator->source);
        return false;
    }

    size_t i = add_symbol(validator, stmt->symbol);
    if (i == (size_t) -1) {
        mtr_report_error(stmt->symbol.token, "Redefinition of name.", validator->source);

        struct mtr_symbol* s = find_symbol(validator, stmt->symbol.token);
        mtr_report_message(s->token, "Previuosly defined here.", validator->source);
        return false;
    }

    stmt->symbol.index = i;
    return true;
}

static struct mtr_stmt* analyze(struct mtr_stmt* stmt, struct validator* validator);

#undef INVALID_RETURN_VALUE
#define INVALID_RETURN_VALUE sanitize_stmt(stmt, false)

static struct mtr_stmt* sanitize_stmt(void* stmt, bool condition) {
    if (!condition) {
        mtr_free_stmt(stmt);
        return NULL;
    }
    return stmt;
}

static struct mtr_stmt* analyze_block(struct mtr_block* block, struct validator* validator) {
    bool all_ok = true;

    size_t current = validator->count;

    for (size_t i = 0; i < block->size; ++i) {
        struct mtr_stmt* s = block->statements[i];
        struct mtr_stmt* checked = analyze(s, validator);
        block->statements[i] = checked;
        all_ok = checked != NULL && all_ok;
    }

    block->var_count = (u16) (validator->count - current);
    return sanitize_stmt(block, all_ok);
}

static struct mtr_stmt* analyze_scope(struct mtr_block* block, struct validator* validator) {
    struct validator scope_validator;
    init_validator(&scope_validator, validator);

    struct mtr_stmt* b = analyze_block(block, &scope_validator);

    delete_validator(&scope_validator);

    return b;
}

static struct mtr_stmt* analyze_variable(struct mtr_variable* decl, struct validator* validator) {
    bool expr = true;
    struct mtr_type* value_type = decl->value == NULL ? NULL : analyze_expr(decl->value, validator);

    if (!decl->symbol.type) {
        decl->symbol.type = value_type;
    }

    if (decl->symbol.type->type == MTR_DATA_STRUCT && !decl->value) {
        struct mtr_struct_type* type = (struct mtr_struct_type*) decl->symbol.type;
        struct mtr_symbol* name = find_symbol(validator, type->name.name);
        MTR_ASSERT(name != NULL, "Type not loaded");

        // Create an expression for the constructor
        struct mtr_primary* primary = malloc(sizeof(struct mtr_primary));
        primary->expr_.type = MTR_EXPR_PRIMARY;
        primary->symbol = *name;

        struct mtr_call* call = malloc(sizeof (struct mtr_call));
        call->expr_.type = MTR_EXPR_CALL;
        call->callable = (struct mtr_expr*) primary;
        call->argv = NULL;
        call->argc = 0;

        decl->value = (struct mtr_expr*)call;
        goto ret;
    }

    if (decl->value) {
        if (!check_assignemnt(decl->symbol.type, value_type)) {
            mtr_report_error(decl->symbol.token, "Invalid assignement to variable of different type", validator->source);
            expr = false;
        }
    }

    if (decl->symbol.type->type == MTR_DATA_INVALID) {
        expr = false;
    }

ret:
    decl->symbol.assignable = true;
    bool loaded = load_var(decl, validator);
    return sanitize_stmt(decl, expr && loaded);
}

static struct mtr_stmt* analyze_function_no_validator(struct mtr_function_decl* stmt, struct validator* validator) {
    bool all_ok = true;

    for (size_t i = 0; i < stmt->argc; ++i) {
        struct mtr_variable* arg = stmt->argv + i;
        all_ok = analyze_variable(arg, validator) && all_ok;
    }

    struct mtr_stmt* checked = analyze(stmt->body, validator);
    stmt->body = checked;

    all_ok = checked != NULL && all_ok;

    struct mtr_function_type* type =  (struct mtr_function_type*) stmt->symbol.type;
    struct mtr_stmt* last = NULL;
    if (stmt->body->type == MTR_STMT_BLOCK) {
        struct mtr_block* body = (struct mtr_block*) stmt->body;
        last = body->statements[body->size-1];
    } else {
        last = stmt->body;
    }

    if (type->return_->type != MTR_DATA_VOID && all_ok) {
        if (last->type != MTR_STMT_RETURN) {
            mtr_report_error(stmt->symbol.token, "Non void function doesn't return anything.", validator->source);
            return false;
        }
    }

    return sanitize_stmt(stmt, all_ok);
}

static struct mtr_stmt* analyze_fn(struct mtr_function_decl* stmt, struct validator* validator) {
    bool all_ok = true;

    struct validator fn_validator;
    init_validator(&fn_validator, validator);
    fn_validator.count = 0;

    struct mtr_stmt* s = analyze_function_no_validator(stmt, &fn_validator);

    delete_validator(&fn_validator);

    return s;
}

static struct mtr_stmt* analyze_assignment(struct mtr_assignment* stmt, struct validator* validator) {
    if (stmt->right->type == MTR_EXPR_PRIMARY) {
        struct mtr_primary* p = (struct mtr_primary*) stmt->right;
        struct mtr_symbol* s = find_symbol(validator, p->symbol.token);
        if (NULL == s) {
            struct mtr_variable* v = malloc(sizeof(struct mtr_variable));
            v->stmt.type = MTR_STMT_VAR;
            v->symbol.token = p->symbol.token;
            v->symbol.type = NULL;
            v->value = stmt->expression;

            mtr_free_expr((struct mtr_expr*) p);
            free(stmt);
            return analyze_variable(v, validator);
        }
    }

    const struct mtr_type* right_t = analyze_expr(stmt->right, validator);
    TYPE_CHECK(right_t);

    // if (!right_t->assignable) {
    //     expr_error(stmt->right, "Expression is not assignable.", validator->source);
    //     return sanitize_stmt(stmt, false);
    // }

    const struct mtr_type* expr_t = analyze_expr(stmt->expression, validator);
    TYPE_CHECK(expr_t);

    bool expr_ok = true;
    if (!check_assignemnt(right_t, expr_t)) {
        expr_error(stmt->right, "Invalid assignement to variable of different type", validator->source);
        expr_ok = false;
    }

    return sanitize_stmt(stmt, expr_ok);
}

static struct mtr_stmt* analyze_if(struct mtr_if* stmt, struct validator* validator) {
    struct mtr_type* expr_type = analyze_expr(stmt->condition, validator);
    TYPE_CHECK(expr_type);

    bool condition_ok = expr_type->type == MTR_DATA_FLOAT || expr_type->type == MTR_DATA_INT || expr_type->type == MTR_DATA_BOOL;
    if (!condition_ok) {
        expr_error(stmt->condition, "Expression doesn't return Bool.", validator->source);
    }

    bool then_ok = true;
    {
        struct validator then;
        init_validator(&then, validator);
        struct mtr_stmt* then_checked = analyze(stmt->then, &then);
        stmt->then = then_checked;
        then_ok = then_checked != NULL;
        delete_validator(&then);
    }

    bool e_ok = true;
    if (stmt->otherwise) {
        struct validator otherwise;
        init_validator(&otherwise, validator);
        struct mtr_stmt* e_checked = analyze(stmt->otherwise, &otherwise);
        stmt->then = e_checked;
        e_ok = e_checked != NULL;
        delete_validator(&otherwise);
    }

    return sanitize_stmt(stmt, condition_ok && then_ok && e_ok);
}

static struct mtr_stmt* analyze_while(struct mtr_while* stmt, struct validator* validator) {
    struct mtr_type* expr_type = analyze_expr(stmt->condition, validator);
    TYPE_CHECK(expr_type);

    bool condition_ok = expr_type->type == MTR_DATA_FLOAT || expr_type->type == MTR_DATA_INT || expr_type->type == MTR_DATA_BOOL;
    if (!condition_ok) {
        expr_error(stmt->condition, "Expression doesn't return Bool.", validator->source);
    }

    struct validator body;
    init_validator(&body, validator);
    struct mtr_stmt* body_checked = analyze(stmt->body, &body);
    stmt->body = body_checked;
    bool body_ok = body_checked != NULL;
    delete_validator(&body);

    return sanitize_stmt(stmt, condition_ok && body_ok);
}

static struct mtr_stmt* analyze_return(struct mtr_return* stmt, struct validator* validator) {
    struct mtr_function_type* t = (struct mtr_function_type*) stmt->from->symbol.type;
    struct mtr_type* type = t->return_;;

    struct mtr_type* expr_type = analyze_expr(stmt->expr, validator);
    TYPE_CHECK(expr_type);

    bool ok = expr_type == type;
    if (!ok) {
        expr_error(stmt->expr, "Incompatible return type.", validator->source);
        mtr_report_message(stmt->from->symbol.token, "As declared here.", validator->source);
        return sanitize_stmt(stmt, false);
    }
    return (struct mtr_stmt*) stmt;
}

static struct mtr_stmt* analyze_call_stmt(struct mtr_call_stmt* call, struct validator* validator) {
    struct mtr_type* type = analyze_expr(call->call, validator);
    return sanitize_stmt(call, type != NULL);
}

// static struct mtr_stmt* analyze_union(struct mtr_union_decl* u, struct validator* validator) {
//     return (struct mtr_stmt*) u;
// }

static struct mtr_stmt* analyze_closure(struct mtr_closure_decl* closure, struct validator* validator) {
    size_t i = add_symbol(validator, closure->function->symbol);
    if (i == (size_t) -1) {
        mtr_report_error(closure->function->symbol.token, "Redefinition of name.", validator->source);

        struct mtr_symbol* s = find_symbol(validator, closure->function->symbol.token);
        mtr_report_message(s->token, "Previuosly defined here.", validator->source);
        return sanitize_stmt(closure, false);
    }

    closure->function->symbol.index = i;

    struct validator cl_validator;
    init_validator(&cl_validator, validator);
    cl_validator.closure = closure;
    cl_validator.count = 0;
    closure->function = (struct mtr_function_decl*) analyze_function_no_validator(closure->function, &cl_validator);
    delete_validator(&cl_validator);

    return sanitize_stmt(closure, closure->function != NULL);
}

static struct mtr_stmt* analyze_struct(struct mtr_struct_decl* s, struct validator* validator) {
    bool all_ok = true;

    struct validator st_validator;
    init_validator(&st_validator, validator);

    for (size_t i = 0; i < s->argc; ++i) {
        struct mtr_variable* var = s->members[i];
        struct mtr_variable* checked = (struct mtr_variable*) analyze_variable(var, &st_validator);
        s->members[i] = checked;
        all_ok = checked != NULL && all_ok;
    }

    delete_validator(&st_validator);

    return sanitize_stmt(s, all_ok);
}

static struct mtr_stmt* analyze(struct mtr_stmt* stmt, struct validator* validator) {
    switch (stmt->type)
    {
    case MTR_STMT_SCOPE:      return analyze_scope((struct mtr_block*) stmt, validator);
    case MTR_STMT_BLOCK:      return analyze_block((struct mtr_block*) stmt, validator);
    case MTR_STMT_ASSIGNMENT: return analyze_assignment((struct mtr_assignment*) stmt, validator);
    case MTR_STMT_FN:         return analyze_fn((struct mtr_function_decl*) stmt, validator);
    case MTR_STMT_VAR:        return analyze_variable((struct mtr_variable*) stmt, validator);
    case MTR_STMT_IF:         return analyze_if((struct mtr_if*) stmt, validator);
    case MTR_STMT_WHILE:      return analyze_while((struct mtr_while*) stmt, validator);
    case MTR_STMT_RETURN:     return analyze_return((struct mtr_return*) stmt, validator);
    case MTR_STMT_CALL:       return analyze_call_stmt((struct mtr_call_stmt*) stmt, validator);
    case MTR_STMT_STRUCT:     return analyze_struct((struct mtr_struct_decl*) stmt, validator);
    case MTR_STMT_CLOSURE:    return analyze_closure((struct mtr_closure_decl*) stmt, validator);

    case MTR_STMT_UNION:
    case MTR_STMT_NATIVE_FN:
        return stmt;
    }
    MTR_ASSERT(false, "Invalid stmt type.");
    return false;
}

static struct mtr_stmt* global_analysis(struct mtr_stmt* stmt, struct validator* validator) {
    switch (stmt->type)
    {
    case MTR_STMT_NATIVE_FN: return stmt;
    case MTR_STMT_FN: return analyze_fn((struct mtr_function_decl*) stmt, validator);
    case MTR_STMT_UNION: return stmt;
    case MTR_STMT_STRUCT: return analyze_struct((struct mtr_struct_decl*) stmt, validator);
    default:
        break;
    }
    MTR_ASSERT(false, "Invalid stmt type.");
    return NULL;
}

static bool load_union(struct mtr_union_decl* u, struct validator* validator) {
    size_t i = add_symbol(validator, u->symbol);
    if (i == (size_t) -1) {
        mtr_report_error(u->symbol.token, "Redefinition of name.", validator->source);

        struct mtr_symbol* s = find_symbol(validator, u->symbol.token);
        mtr_report_message(s->token, "Previuosly defined here.", validator->source);
        return false;
    }

    u->symbol.index = i;
    return true;
}

static bool load_struct(struct mtr_struct_decl* st, struct validator* validator) {
    size_t i = add_symbol(validator, st->symbol);
    if (i == (size_t) -1) {
        mtr_report_error(st->symbol.token, "Redefinition of name.", validator->source);

        struct mtr_symbol* s = find_symbol(validator, st->symbol.token);
        mtr_report_message(s->token, "Previuosly defined here.", validator->source);
        return false;
    }

    st->symbol.index = i;
    return true;
}

static bool load_native_fn(struct mtr_function_decl* stmt, struct validator* validator) {
    size_t i = add_symbol(validator, stmt->symbol);
    if (i == (size_t) -1) {
        mtr_report_error(stmt->symbol.token, "Redefinition of name. (Native functions are not overloadable).", validator->source);

        struct mtr_symbol* s = find_symbol(validator, stmt->symbol.token);
        mtr_report_message(s->token, "Previuosly defined here.", validator->source);
        return false;
    }

    stmt->symbol.index = i;
    return true;
}

static bool load_global(struct mtr_stmt* stmt, struct validator* validator) {
    switch (stmt->type)
    {
    case MTR_STMT_NATIVE_FN:
        return load_native_fn((struct mtr_function_decl*) stmt, validator);
    case MTR_STMT_FN:
        return load_fn((struct mtr_function_decl*) stmt, validator);
    case MTR_STMT_UNION:
        return load_union((struct mtr_union_decl*) stmt, validator);
    case MTR_STMT_STRUCT:
        return load_struct((struct mtr_struct_decl*) stmt, validator);
    default:
        break;
    }
    MTR_ASSERT(false, "Invalid stmt type.");
    return false;
}

bool mtr_validate(struct mtr_ast* ast) {
    struct validator validator;
    validator.closure = NULL;
    mtr_init_symbol_table(&validator.symbols);
    validator.count = 0;
    validator.enclosing = NULL;
    validator.source = ast->source;
    validator.type_list = &ast->type_list;

    bool all_ok = true;

    struct mtr_block* block = (struct mtr_block*) ast->head;

    for (size_t i = 0; i < block->size; ++i) {
        struct mtr_stmt* s = block->statements[i];
        all_ok = load_global(s, &validator);
    }

    for (size_t i = 0; i < block->size; ++i) {
        struct mtr_stmt* s = block->statements[i];
        struct mtr_stmt* checked = global_analysis(s, &validator);
        block->statements[i] = checked;
        all_ok =  checked != NULL && all_ok;
    }

    delete_validator(&validator);
    return all_ok;
}
