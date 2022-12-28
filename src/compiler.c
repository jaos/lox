#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chunk.h"
#include "common.h"
#include "compiler.h"
#ifdef DEBUG
#include "debug.h"
#endif
#include "memory.h"
#include "object.h"
#include "scanner.h"

typedef struct {
    token_t current;
    token_t previous;
    bool had_error;
    bool panic_mode;
} parser_t;

typedef enum {
    PREC_NONE,
    PREC_ASSIGNMENT, // =
    PREC_OR,         // or
    PREC_AND,        // and
    PREC_EQUALITY,   // == !=
    PREC_COMPARISON, // < > <= >=
    PREC_TERM,       // + -
    PREC_FACTOR,     // * /
    PREC_UNARY,      // ! -
    PREC_CALL,       //.  ()
    PREC_PRIMARY,
} precedence_t;

typedef void (*parse_fn_t)(const bool);

typedef struct {
    parse_fn_t prefix;
    parse_fn_t infix;
    precedence_t precedence;
} parse_rule_t;

typedef struct {
    token_t name;
    int depth;
    bool is_captured;
} local_t;

typedef struct {
    uint8_t index;
    bool is_local;
} upvalue_t;

typedef enum {
    TYPE_FUNCTION,
    TYPE_INITIALIZER,
    TYPE_METHOD,
    TYPE_SCRIPT,
} function_type_t;

typedef struct compiler {
    struct compiler *enclosing;
    obj_function_t *function;
    function_type_t type;
    local_t locals[UINT8_COUNT];
    int local_count;
    upvalue_t upvalues[UINT8_COUNT];
    int scope_depth;
} compiler_t;

typedef struct class_compiler {
    struct class_compiler *enclosing;
    bool has_superclass;
} class_compiler_t;

static parser_t parser;
// static table_t string_constants; // chapter 21 challenge
static compiler_t *current = NULL;
static class_compiler_t *current_class = NULL;
static int compiler_count = 0;

#define MAX_COMPILERS 1024
#define MAX_PARAMETERS 255

static chunk_t *current_chunk(void)
{
    return &current->function->chunk;
}

static void error_at(const token_t *token, const char *message)
{
    if (parser.panic_mode) return;

    parser.panic_mode = true;
    fprintf(stderr, gettext("[line %d] Error"), token->line);
    if (token->type == TOKEN_EOF) {
        fprintf(stderr, gettext(" at end"));
    } else if (token->type == TOKEN_ERROR) {
        // nothing
    } else {
        fprintf(stderr, gettext(" at '%.*s'"), token->length, token->start);
    }

    fprintf(stderr, ": %s\n", message);
    parser.had_error = true;
}

static void error(const char *message)
{
    error_at(&parser.previous, message);
}

static void error_at_current(const char *message)
{
    error_at(&parser.current, message);
}

static void advance(void)
{
    parser.previous = parser.current;
    for (;;) {
        parser.current = scan_token();
        if (parser.current.type != TOKEN_ERROR) break;
        error_at_current(parser.current.start);
    }
}

static void consume(const token_type_t type, const char *message)
{
    if (parser.current.type == type) {
        advance();
        return;
    }
    error_at_current(message);
}

static bool check(const token_type_t type)
{
    return parser.current.type == type;
}

static bool match(const token_type_t type)
{
    if (!check(type))
        return false;
    advance();
    return true;
}

static void emit_byte(const uint8_t byte)
{
    write_chunk(current_chunk(), byte, parser.previous.line);
}

static void emit_bytes(const uint8_t byte1, const uint8_t byte2)
{
    emit_byte(byte1);
    emit_byte(byte2);
}

static void emit_loop(const int loop_start)
{
    emit_byte(OP_LOOP);

    const int offset = current_chunk()->count - loop_start + 2;
    if (offset > UINT16_MAX)
        error(gettext("Loop body too large."));

    emit_byte((offset >> 8) & 0xff);
    emit_byte(offset & 0xff);
}

static int emit_jump(const uint8_t instruction)
{
    emit_byte(instruction);
    emit_byte(0xff);
    emit_byte(0xff);
    return current_chunk()->count - 2;
}

static void emit_return(void)
{
    if (current->type == TYPE_INITIALIZER) {
        emit_bytes(OP_GET_LOCAL, 0); // make sure the instance from slot zero is left on the stack
    } else {
        emit_byte(OP_NIL);
    }
    emit_byte(OP_RETURN);
}

static uint8_t make_constant(const value_t value)
{
    const int constant = add_constant(current_chunk(), value);
    if (constant > UINT8_MAX) {
        error(gettext("Too many constants in one chunk.")); // See OP_CONSTANT_LONG to fix
        return 0;
    }
    return (uint8_t)constant;
}

static void emit_constant(const value_t value)
{
    emit_bytes(OP_CONSTANT, make_constant(value));
}

static void patch_jump(const int offset)
{
    const int jump = current_chunk()->count - offset - 2;
    if (jump > UINT16_MAX) {
        error(gettext("Too much code to jump over."));
    }
    current_chunk()->code[offset] = (jump >> 8) & 0xff;
    current_chunk()->code[offset + 1] = jump & 0xff;
}

static void init_compiler(compiler_t *compiler, const function_type_t type)
{
    if (compiler_count >= MAX_COMPILERS) {
        fprintf(stderr, gettext("Too many compilers."));
        exit(EXIT_FAILURE);
    }
    compiler_count++;
    compiler->enclosing = current;
    compiler->function = NULL;
    compiler->type = type;
    compiler->local_count = 0;
    compiler->scope_depth = 0;
    compiler->function = new_obj_function_t();
    current = compiler;

    if (type != TYPE_SCRIPT) {
        current->function->name = copy_string(parser.previous.start, parser.previous.length);
    }

    local_t *local = &current->locals[current->local_count++];
    local->depth = 0;
    local->is_captured = false;
    if (type != TYPE_FUNCTION) {
        local->name.start = token_keyword_names[TOKEN_THIS];
        local->name.length = TOKEN_THIS_LEN;
      } else {
        local->name.start = "";
        local->name.length = 0;
    }
}

static obj_function_t *end_compiler(void)
{
    emit_return();
    obj_function_t *function = current->function;
    #ifdef DEBUG
    if (!parser.had_error) {
        disassemble_chunk(current_chunk(), function->name != NULL ? function->name->chars : "<script>");
    }
    #endif
    current = current->enclosing;
    compiler_count--;
    return function;
}

static void begin_scope(void)
{
    current->scope_depth++;
}

static void end_scope(void)
{
    current->scope_depth--;

    /*
    */
    while (current->local_count > 0 && current->locals[current->local_count - 1].depth > current->scope_depth) {
        if (current->locals[current->local_count - 1].is_captured) {
            emit_byte(OP_CLOSE_UPVALUE);
        } else {
            emit_byte(OP_POP);
        }
        current->local_count--;
    }

    /* From sidebar: implement a OP_POPN instruction
    uint8_t local_count = current->local_count;
    while (local_count > 0 && current->locals[local_count - 1].depth > current->scope_depth) {
        local_count--;
    }
    emit_bytes(OP_POPN, current->local_count - local_count);
    current->local_count = local_count;
    */
}

static void expression(void);
static void statement(void);
static void declaration(void);
static const parse_rule_t *get_rule(const token_type_t type);
static void parse_precedence(const precedence_t precedence);

static uint8_t identifier_constant(const token_t *name)
{
    /* NOTE: chapter 21 challenge
    obj_string_t *constant_str = copy_string(name->start, name->length);
    value_t existing_index;
    if (get_table_t(&string_constants, constant_str, &existing_index)) {
        return (uint8_t)AS_NUMBER(existing_index);
    }
    uint8_t index = make_constant(OBJ_VAL(constant_str));
    set_table_t(&string_constants, constant_str, NUMBER_VAL(index));
    return index;
    */
    return make_constant(OBJ_VAL(copy_string(name->start, name->length)));
}

static bool identifiers_equal(const token_t *a, const token_t *b)
{
    if (a->length != b->length)
        return false;
    return memcmp(a->start, b->start, a->length) == 0;
}

static int resolve_local(compiler_t *compiler, const token_t *name)
{
    for (int i = compiler->local_count - 1; i >= 0; i--) {
        const local_t *local = &compiler->locals[i];
        if (identifiers_equal(name, &local->name)) {
            if (local->depth == -1) {
                error(gettext("Can't read local variable in its own initializer."));
            }
            return i;
        }
    }
    return -1;
}

static int add_upvalue(compiler_t *compiler, const uint8_t index, const bool is_local)
{
    int upvalue_count = compiler->function->upvalue_count;
    for (int i = 0; i < upvalue_count; i++) {
        upvalue_t *upvalue = &compiler->upvalues[i];
        if (upvalue->index == index && upvalue->is_local == is_local) {
            return i;
        }
    }
    if (upvalue_count == UINT8_COUNT) {
        error(gettext("Too many closure variables in function."));
        return 0;
    }
    compiler->upvalues[upvalue_count].is_local = is_local;
    compiler->upvalues[upvalue_count].index = index;
    return compiler->function->upvalue_count++;
}

static int resolve_upvalue(compiler_t *compiler, const token_t *name)
{
    if (compiler->enclosing == NULL)
        return -1; // must be global

    // try local
    const int local = resolve_local(compiler->enclosing, name);
    if (local != -1) {
        compiler->enclosing->locals[local].is_captured = true;
        return add_upvalue(compiler, (uint8_t)local, true);
    }

    // try enclosing... in which case every enclosing scope will have
    // it's own upvalue that the enclosed scopes then reference.
    const int upvalue = resolve_upvalue(compiler->enclosing, name);
    if (upvalue != -1) {
        return add_upvalue(compiler, (uint8_t)upvalue, false);
    }

    return -1;
}

static void add_local(const token_t name)
{
    if (current->local_count == UINT8_COUNT) {
        error(gettext("Too many local variables in function."));
        return;
    }
    local_t *local = &current->locals[current->local_count++];
    local->name = name;
    // NOTE here use use a depth of -1 to indicate uninitialized, see mark_initialized and resolve_local
    // local->depth = current->scope_depth;
    local->depth = -1;
    local->is_captured = false;
}

static void declare_variable(void)
{
    if (current->scope_depth == 0)
        return; // handled by defining globals elsewhere
    const token_t *name = &parser.previous;

    for (int i = current->local_count - 1; i >= 0; i--) {
        local_t *local = &current->locals[i];
        if (local->depth != -1 && local->depth < current->scope_depth) {
            break;
        }
        if (identifiers_equal(name, &local->name)) {
            error(gettext("Already a variable with this name in this scope."));
        }
    }
    add_local(*name);
}

static uint8_t parse_variable(const char *message)
{
    consume(TOKEN_IDENTIFIER, message);
    declare_variable();
    if (current->scope_depth > 0) {
        return 0; // dummy index since this isn't a constant
    }
    return identifier_constant(&parser.previous);
}

static void mark_initialized(void)
{
    if (current->scope_depth == 0) {
        return;
    }
    current->locals[current->local_count - 1].depth = current->scope_depth;
}

static void define_variable(const uint8_t variable)
{
    if (current->scope_depth > 0) {
        mark_initialized();
        return;
    }
    emit_bytes(OP_DEFINE_GLOBAL, variable);
}

static uint8_t argument_list(void)
{
    uint8_t arg_count = 0;
    if (!check(TOKEN_RIGHT_PAREN)) {
        do {
            expression();
            if (arg_count >= MAX_PARAMETERS) {
                error(gettext("Can't have more than " CPP_STRINGIFY(MAX_PARAMETERS) " arguments."));
            }
            arg_count++;
        } while (match(TOKEN_COMMA));
    }
    consume(TOKEN_RIGHT_PAREN, gettext("Expect ')' after arguments."));
    return arg_count;
}

static void and_expr(const bool) // can_assign
{
    const int end_jump = emit_jump(OP_JUMP_IF_FALSE);
    emit_byte(OP_POP);

    parse_precedence(PREC_AND);

    patch_jump(end_jump);
}

static void binary(const bool)
{
    token_type_t operator_type = parser.previous.type;
    const parse_rule_t *rule = get_rule(operator_type);
    parse_precedence((precedence_t)(rule->precedence + 1));
    switch (operator_type) {
        case TOKEN_BANG_EQUAL: emit_bytes(OP_EQUAL, OP_NOT); break;
        case TOKEN_EQUAL_EQUAL: emit_byte(OP_EQUAL); break;
        case TOKEN_GREATER: emit_byte(OP_GREATER); break;
        case TOKEN_GREATER_EQUAL: emit_bytes(OP_LESS, OP_NOT); break;
        case TOKEN_LESS: emit_byte(OP_LESS); break;
        case TOKEN_LESS_EQUAL: emit_bytes(OP_GREATER, OP_NOT); break;
        case TOKEN_PLUS: emit_byte(OP_ADD); break;
        case TOKEN_MINUS: emit_byte(OP_SUBTRACT); break;
        case TOKEN_STAR: emit_byte(OP_MULTIPLY); break;
        case TOKEN_SLASH: emit_byte(OP_DIVIDE); break;
        default: return; // unreachable
    }
}

static void call(const bool)
{
    const uint8_t arg_count = argument_list();
    emit_bytes(OP_CALL, arg_count);
}

static void dot(const bool can_assign)
{
    consume(TOKEN_IDENTIFIER, gettext("Expect property name after '.'."));
    const uint8_t name = identifier_constant(&parser.previous);

    if (can_assign && match(TOKEN_EQUAL)) {
        expression();
        emit_bytes(OP_SET_PROPERTY, name);
    } else if (match(TOKEN_LEFT_PAREN)) { // optimization here since we are immediately calling the method
        const uint8_t arg_count = argument_list();
        emit_bytes(OP_INVOKE, name);
        emit_byte(arg_count);
    } else {
        emit_bytes(OP_GET_PROPERTY, name);
    }
}

static void literal(const bool)
{
    switch (parser.previous.type) {
        case TOKEN_FALSE: emit_byte(OP_FALSE); break;
        case TOKEN_NIL: emit_byte(OP_NIL); break;
        case TOKEN_TRUE: emit_byte(OP_TRUE); break;
        default: return; // unreachable
    }
}

static void grouping(const bool)
{
    expression();
    consume(TOKEN_RIGHT_PAREN, gettext("Expect ')' after expression."));
}

static void number(const bool)
{
    const double value = strtod(parser.previous.start, NULL);
    emit_constant(NUMBER_VAL(value));
}

static void or_expr(const bool) // can_assign
{
    const int else_jump = emit_jump(OP_JUMP_IF_FALSE);
    const int end_jump = emit_jump(OP_JUMP);

    patch_jump(else_jump);
    emit_byte(OP_POP);

    parse_precedence(PREC_OR);
    patch_jump(end_jump);
}

static void string(const bool)
{
    emit_constant(OBJ_VAL(copy_string(parser.previous.start + 1, parser.previous.length - 2)));
}

static void named_variable(const token_t name, const bool can_assign)
{
    uint8_t get_op, set_op;
    int arg = resolve_local(current, &name);
    if (arg != -1) {
        get_op = OP_GET_LOCAL;
        set_op = OP_SET_LOCAL;
    } else if ((arg = resolve_upvalue(current, &name)) != -1){
        get_op = OP_GET_UPVALUE;
        set_op = OP_SET_UPVALUE;
    } else {
        arg = identifier_constant(&name);
        get_op = OP_GET_GLOBAL;
        set_op = OP_SET_GLOBAL;
    }

    if (can_assign && match(TOKEN_EQUAL)) {
        expression();
        emit_bytes(set_op, (uint8_t)arg);
    } else {
        emit_bytes(get_op, (uint8_t)arg);
    }
}

static void variable(const bool can_assign)
{
    named_variable(parser.previous, can_assign);
}

static token_t synthetic_token(const char *text)
{
    token_t token;
    token.start = text;
    token.length = strlen(text);
    return token;
}

static void super_expr(const bool)
{
    if (current_class == NULL) {
        error(gettext("Can't use 'super' outside of a class."));
    } else if (!current_class->has_superclass) {
        error(gettext("Can't use 'super' in a class with no superclass."));
    }

    consume(TOKEN_DOT, gettext("Expect '.' after 'super'."));
    consume(TOKEN_IDENTIFIER, gettext("Expect superclass method name."));
    const uint8_t method_name = identifier_constant(&parser.previous);

    // capture this and super in case we are in a closure
    named_variable(synthetic_token(token_keyword_names[TOKEN_THIS]), false);

    // try a fast path
    if (match(TOKEN_LEFT_PAREN)) {
        const uint8_t arg_count = argument_list();
        named_variable(synthetic_token(token_keyword_names[TOKEN_SUPER]), false);
        emit_bytes(OP_SUPER_INVOKE, method_name);
        emit_byte(arg_count);
    } else { // slow path
        named_variable(synthetic_token(token_keyword_names[TOKEN_SUPER]), false);
        emit_bytes(OP_GET_SUPER, method_name);
    }
}

static void this_expr(const bool)
{
    // check we have a setup from class_declaration
    if (current_class == NULL) {
        error(gettext("Can't use 'this' outside of a class."));
        return;
    }
    variable(false);
}

static void unary(const bool)
{
    const token_type_t operator_type = parser.previous.type;

    // compile the operand
    parse_precedence(PREC_UNARY);

    // emit the operator instruction
    switch (operator_type) {
        case TOKEN_BANG: emit_byte(OP_NOT); break;
        case TOKEN_MINUS: emit_byte(OP_NEGATE); break;
        default: return; // unreachable
    }
}

const parse_rule_t rules[] = {
    [TOKEN_LEFT_PAREN]      = {grouping,    call,   PREC_CALL},
    [TOKEN_RIGHT_PAREN]     = {NULL,        NULL,   PREC_NONE},
    [TOKEN_LEFT_BRACE]      = {NULL,        NULL,   PREC_NONE},
    [TOKEN_RIGHT_BRACE]     = {NULL,        NULL,   PREC_NONE},
    [TOKEN_COMMA]           = {NULL,        NULL,   PREC_NONE},
    [TOKEN_DOT]             = {NULL,        dot,    PREC_CALL},
    [TOKEN_MINUS]           = {unary,       binary, PREC_TERM},
    [TOKEN_PLUS]            = {NULL,        binary, PREC_TERM},
    [TOKEN_SEMICOLON]       = {NULL,        NULL,   PREC_NONE},
    [TOKEN_SLASH]           = {NULL,        binary, PREC_FACTOR},
    [TOKEN_STAR]            = {NULL,        binary, PREC_FACTOR},
    [TOKEN_BANG]            = {unary,       NULL,   PREC_NONE},
    [TOKEN_BANG_EQUAL]      = {NULL,        binary, PREC_EQUALITY},
    [TOKEN_EQUAL]           = {NULL,        NULL,   PREC_NONE},
    [TOKEN_EQUAL_EQUAL]     = {NULL,        binary, PREC_EQUALITY},
    [TOKEN_GREATER]         = {NULL,        binary, PREC_COMPARISON},
    [TOKEN_GREATER_EQUAL]   = {NULL,        binary, PREC_COMPARISON},
    [TOKEN_LESS]            = {NULL,        binary, PREC_COMPARISON},
    [TOKEN_LESS_EQUAL]      = {NULL,        binary, PREC_COMPARISON},
    [TOKEN_IDENTIFIER]      = {variable,    NULL,   PREC_NONE},
    [TOKEN_STRING]          = {string,      NULL,   PREC_NONE},
    [TOKEN_NUMBER]          = {number,      NULL,   PREC_NONE},
    [TOKEN_AND]             = {NULL,        and_expr,PREC_AND},
    [TOKEN_CLASS]           = {NULL,        NULL,   PREC_NONE},
    [TOKEN_ELSE]            = {NULL,        NULL,   PREC_NONE},
    [TOKEN_FALSE]           = {literal,     NULL,   PREC_NONE},
    [TOKEN_FOR]             = {NULL,        NULL,   PREC_NONE},
    [TOKEN_FUN]             = {NULL,        NULL,   PREC_NONE},
    [TOKEN_IF]              = {NULL,        NULL,   PREC_NONE},
    [TOKEN_NIL]             = {literal,     NULL,   PREC_NONE},
    [TOKEN_OR]              = {NULL,        or_expr,PREC_OR},
    [TOKEN_PRINT]           = {NULL,        NULL,   PREC_NONE},
    [TOKEN_RETURN]          = {NULL,        NULL,   PREC_NONE},
    [TOKEN_SUPER]           = {super_expr,  NULL,   PREC_NONE},
    [TOKEN_THIS]            = {this_expr,   NULL,   PREC_NONE},
    [TOKEN_TRUE]            = {literal,     NULL,   PREC_NONE},
    [TOKEN_VAR]             = {NULL,        NULL,   PREC_NONE},
    [TOKEN_WHILE]           = {NULL,        NULL,   PREC_NONE},
    [TOKEN_ERROR]           = {NULL,        NULL,   PREC_NONE},
    [TOKEN_EOF]             = {NULL,        NULL,   PREC_NONE},
};

static void parse_precedence(const precedence_t precedence)
{
    advance();
    const parse_fn_t prefix_rule = get_rule(parser.previous.type)->prefix;
    if (prefix_rule == NULL) {
        error(gettext("Expect expression."));
        return;
    }

    const bool can_assign = precedence <= PREC_ASSIGNMENT;
    prefix_rule(can_assign);

    while (precedence <= get_rule(parser.current.type)->precedence) {
        advance();
        const parse_fn_t infix_rule = get_rule(parser.previous.type)->infix;
        infix_rule(can_assign);
    }

    if (can_assign && match(TOKEN_EQUAL)) {
        error(gettext("Invalid assignment target."));
    }
}

static const parse_rule_t *get_rule(const token_type_t type)
{
    return &rules[type];
}

static void expression(void)
{
    parse_precedence(PREC_ASSIGNMENT);
}

static void block(void)
{
    while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
        declaration();
    }
    consume(TOKEN_RIGHT_BRACE, gettext("Expect '}' after block."));
}

static void function(function_type_t type)
{
    compiler_t compiler;
    init_compiler(&compiler, type);
    begin_scope();

    consume(TOKEN_LEFT_PAREN, gettext("Expect '(' after function name."));
    if (!check(TOKEN_RIGHT_PAREN)) {
        do {
            current->function->arity++;
            if (current->function->arity > MAX_PARAMETERS) {
                error_at_current("Can't have more than " CPP_STRINGIFY(MAX_PARAMETERS) " parameters.");
            }
            const uint8_t constant = parse_variable("Expect parameter name.");
            define_variable(constant);
        } while (match(TOKEN_COMMA));
    }
    consume(TOKEN_RIGHT_PAREN, gettext("Expect ')' after parameters."));
    consume(TOKEN_LEFT_BRACE, gettext("Expect '{' before function body."));
    block();

    obj_function_t *function = end_compiler(); // no end_scope required here
    emit_bytes(OP_CLOSURE, make_constant(OBJ_VAL(function)));

    for (int i = 0; i < function->upvalue_count; i++) {
        emit_byte(compiler.upvalues[i].is_local ? 1 : 0);
        emit_byte(compiler.upvalues[i].index);
    }
}

static void method(void)
{
    consume(TOKEN_IDENTIFIER, gettext("Expect method name."));
    const uint8_t constant = identifier_constant(&parser.previous);

    function_type_t type = TYPE_METHOD;
    if (parser.previous.length == KEYWORD_INIT_LEN &&
    memcmp(parser.previous.start, token_keyword_names[KEYWORD_INIT], KEYWORD_INIT_LEN) == 0) {
        type = TYPE_INITIALIZER;
    }
    function(type);

    emit_bytes(OP_METHOD, constant);
}

static void class_declaration(void)
{
    consume(TOKEN_IDENTIFIER, gettext("Expect class name."));
    const token_t class_name = parser.previous;
    const uint8_t name_constant = identifier_constant(&parser.previous);
    declare_variable();

    emit_bytes(OP_CLASS, name_constant);
    define_variable(name_constant);

    // setup a class compiler instance while we do the work
    class_compiler_t class_compiler;
    class_compiler.enclosing = current_class;
    class_compiler.has_superclass = false;
    current_class = &class_compiler;

    if (match(TOKEN_LESS)) {
        consume(TOKEN_IDENTIFIER, gettext("Expect superclass name."));
        variable(false);

        if (identifiers_equal(&class_name, &parser.previous)) {
            error(gettext("A class can't inherit from itself."));
        }

        // setup super... make a new scope so each class gets local slot for super, preventing collisions
        begin_scope();
        add_local(synthetic_token(token_keyword_names[TOKEN_SUPER]));
        define_variable(0);

        named_variable(class_name, false);
        emit_byte(OP_INHERIT);
        class_compiler.has_superclass = true;
    }

    named_variable(class_name, false); // get this back on the stack as a named variable before we compile methods

    consume(TOKEN_LEFT_BRACE, gettext("Expect '{' before class body."));
    while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
        // could add fields and other things here besides methods
        method();
    }
    consume(TOKEN_RIGHT_BRACE, gettext("Expect '}' after class body."));

    emit_byte(OP_POP); // done with our class, pop it off the stack

    if (class_compiler.has_superclass) {
        end_scope();
    }

    // pop off the class compiler
    current_class = current_class->enclosing;
}

static void fun_declaration(void)
{
    const uint8_t global = parse_variable("Expect function name.");
    mark_initialized(); // so we can support recursion before we compile the body
    function(TYPE_FUNCTION);
    define_variable(global);
}

static void var_declaration(void)
{
    const uint8_t global = parse_variable("Expect variable name.");
    if (match(TOKEN_EQUAL)) {
        expression();
    } else {
        emit_byte(OP_NIL);
    }

    consume(TOKEN_SEMICOLON, gettext("Expect ';' after variable declaration."));
    define_variable(global);
}

static void expression_statement(void)
{
    expression();
    consume(TOKEN_SEMICOLON, gettext("Expect ';' after expression."));
    emit_byte(OP_POP);
}

int inner_most_loop_start = -1;
int inner_most_loop_end = -1;
int inner_most_loop_scope_depth = 0;

static void for_statement(void)
{
    int surrounding_loop_start = inner_most_loop_start;
    //int surrounding_loop_end = inner_most_loop_end;
    int surrounding_loop_scope_depth = inner_most_loop_scope_depth;

    begin_scope();

    consume(TOKEN_LEFT_PAREN, gettext("Expect '(' after 'for'."));
    if (match(TOKEN_SEMICOLON)) {
        // no loop initializer
    } else if (match(TOKEN_VAR)) {
        var_declaration();
    } else {
        expression_statement();
    }

    inner_most_loop_start = current_chunk()->count;
    inner_most_loop_scope_depth = current->scope_depth;

    int exit_jump = -1;
    if (!match(TOKEN_SEMICOLON)) { // loop condition
        expression();
        consume(TOKEN_SEMICOLON, gettext("Expect ';' after loop condition."));

        // jump out of the loop if the condition is false
        exit_jump = emit_jump(OP_JUMP_IF_FALSE);
        emit_byte(OP_POP); // condition
    }
    //inner_most_loop_end = exit_jump;

    if (!match(TOKEN_RIGHT_PAREN)) { // increment clause
        int body_jump = emit_jump(OP_JUMP);
        int increment_start = current_chunk()->count;

        expression();

        emit_byte(OP_POP);
        consume(TOKEN_RIGHT_PAREN, gettext("Expect ')' after for clauses."));

        emit_loop(inner_most_loop_start);
        inner_most_loop_start = increment_start;
        patch_jump(body_jump);
    }

    statement();
    emit_loop(inner_most_loop_start);

    if (exit_jump != -1) {
        patch_jump(exit_jump);
        emit_byte(OP_POP);
    }

    inner_most_loop_start = surrounding_loop_start;
    // inner_most_loop_end = surrounding_loop_end;
    inner_most_loop_scope_depth = surrounding_loop_scope_depth;
    end_scope();
}

static void if_statement(void)
{
    consume(TOKEN_LEFT_PAREN, gettext("Expect '(' after 'if'."));
    expression();
    consume(TOKEN_RIGHT_PAREN, gettext("Expect ')' after condition."));

    const int then_jump = emit_jump(OP_JUMP_IF_FALSE);
    emit_byte(OP_POP);
    statement();

    const int else_jump = emit_jump(OP_JUMP);

    patch_jump(then_jump);

    emit_byte(OP_POP);

    if (match(TOKEN_ELSE))
        statement();

    patch_jump(else_jump);
}

static void print_statement(void)
{
    expression();
    consume(TOKEN_SEMICOLON, gettext("Expect ';' after value."));
    emit_byte(OP_PRINT);
}

static void return_statement(void)
{
    if (current->type == TYPE_SCRIPT) {
        error(gettext("Can't return from top-level code."));
    }

    if (match(TOKEN_SEMICOLON)) {
        emit_return();
    } else {
        if (current->type == TYPE_INITIALIZER) {
            error(gettext("Can't return a value from an initializer."));
            // let it continue from here so that the compiler can synchronize
        }
        expression();
        consume(TOKEN_SEMICOLON, gettext("Expect ';' after return value."));
        emit_byte(OP_RETURN);
    }
}

static void while_statement(void)
{
    int surrounding_loop_start = inner_most_loop_start;
    int surrounding_loop_scope_depth = inner_most_loop_scope_depth;

    inner_most_loop_start = current_chunk()->count;
    inner_most_loop_scope_depth = current->scope_depth;

    consume(TOKEN_LEFT_PAREN, gettext("Expect '(' after 'while'."));
    expression();
    consume(TOKEN_RIGHT_PAREN, gettext("Expect ')' after condition."));

    const int exit_jump = emit_jump(OP_JUMP_IF_FALSE);
    emit_byte(OP_POP);
    statement();

    emit_loop(inner_most_loop_start);

    patch_jump(exit_jump);
    emit_byte(OP_POP);

    inner_most_loop_start = surrounding_loop_start;
    inner_most_loop_scope_depth = surrounding_loop_scope_depth;
}

static void synchronize(void)
{
    parser.panic_mode = false;
    while (parser.current.type != TOKEN_EOF) {
        if (parser.previous.type == TOKEN_SEMICOLON)
            return;
        switch (parser.current.type) {
            case TOKEN_CLASS:
            case TOKEN_FUN:
            case TOKEN_VAR:
            case TOKEN_FOR:
            case TOKEN_IF:
            case TOKEN_WHILE:
            case TOKEN_PRINT:
            case TOKEN_RETURN:
                return;
            default:
                ; // do nothing
        }
        advance();
    }
}

static void declaration(void)
{
    if (match(TOKEN_CLASS)) {
        class_declaration();
    } else if (match(TOKEN_FUN)) {
        fun_declaration();
    } else if (match(TOKEN_VAR)) {
        var_declaration();
    } else {
        statement();
    }
    if (parser.panic_mode) synchronize();
}

#define MAX_CASES 256

// NOT IN LOX
static void switch_statement(void)
{
    consume(TOKEN_LEFT_PAREN, gettext("Expect '(' after 'switch'."));
    expression();
    consume(TOKEN_RIGHT_PAREN, gettext("Expect ')' after value."));
    consume(TOKEN_LEFT_BRACE, gettext("Expect '{' before switch cases."));

    int state = 0; // 0: before all cases, 1: before default, 2: after default.
    int case_ends[MAX_CASES];
    int case_count = 0;
    int previous_case_skip = -1;

    while (!match(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
        if (match(TOKEN_CASE) || match(TOKEN_DEFAULT)) {
            token_type_t case_type = parser.previous.type;

            if (state == 2) {
                error(gettext("Can't have another case or default after the default case."));
            }

            if (state == 1) {
                // At the end of the previous case, jump over the others.
                case_ends[case_count++] = emit_jump(OP_JUMP);

                // Patch its condition to jump to the next case (this one).
                patch_jump(previous_case_skip);
                emit_byte(OP_POP);
            }

            if (case_type == TOKEN_CASE) {
                state = 1;

                // See if the case is equal to the value.
                emit_byte(OP_DUP);
                expression();

                consume(TOKEN_COLON, gettext("Expect ':' after case value."));

                emit_byte(OP_EQUAL);
                previous_case_skip = emit_jump(OP_JUMP_IF_FALSE);

                // Pop the comparison result.
                emit_byte(OP_POP);
            } else {
                state = 2;
                consume(TOKEN_COLON, gettext("Expect ':' after default."));
                previous_case_skip = -1;
            }
        } else {
            // Otherwise, it's a statement inside the current case.
            if (state == 0) {
                error(gettext("Can't have statements before any case."));
            }
            statement();
        }
    }

    // If we ended without a default case, patch its condition jump.
    if (state == 1) {
        patch_jump(previous_case_skip);
        //emit_byte(OP_POP); // book says to do this... but we pop one too many if this calls
    }

    // Patch all the case jumps to the end.
    for (int i = 0; i < case_count; i++) {
        patch_jump(case_ends[i]);
    }

    emit_byte(OP_POP); // The switch value.
}

// NOT IN LOX
static void break_statement(void)
{
    assert("This does not work!");
   if (inner_most_loop_end == -1) {
        error(gettext("Can't use 'break' outside of a loop."));
    }
    consume(TOKEN_SEMICOLON, gettext("Expect ';' after 'break'."));
    for (int i = current->local_count - 1; i >=0 && current->locals[i].depth > inner_most_loop_scope_depth; i--) {
        emit_byte(OP_POP);
    }
    emit_loop(inner_most_loop_end);
}

// NOT IN LOX
static void continue_statement(void)
{
    if (inner_most_loop_start == -1) {
        error(gettext("Can't use 'continue' outside of a loop."));
    }
    consume(TOKEN_SEMICOLON, gettext("Expect ';' after 'continue'."));

    /*
    */
    for (int i = current->local_count - 1; i >=0 && current->locals[i].depth > inner_most_loop_scope_depth; i--) {
        emit_byte(OP_POP);
    }

    /* From sidebar: implment a OP_POPN instruction
    uint8_t local_count = current->local_count;
    while (local_count > 0 && current->locals[local_count].depth > inner_most_loop_scope_depth) {
        local_count--;
    }
    emit_bytes(OP_POPN, current->local_count - local_count);
    emit_loop(inner_most_loop_start);
    */
}

static void statement(void)
{
    if (match(TOKEN_PRINT)) {
        print_statement();
    } else if (match(TOKEN_FOR)) {
        for_statement();
    } else if (match(TOKEN_IF)) {
        if_statement();
    } else if (match(TOKEN_RETURN)) {
        return_statement();
    } else if (match(TOKEN_WHILE)) {
        while_statement();
    } else if (match(TOKEN_LEFT_BRACE)) {
        begin_scope();
        block();
        end_scope();
    // not in lox
    } else if (match(TOKEN_SWITCH)) {
        switch_statement();
    } else if (match(TOKEN_BREAK)) {
        break_statement();
    } else if (match(TOKEN_CONTINUE)) {
        continue_statement();
    } else {
        expression_statement();
    }
}

obj_function_t *compile(const char *source)
{
    init_scanner(source);

    compiler_t compiler;
    init_compiler(&compiler, TYPE_SCRIPT);

    parser.had_error = false;
    parser.panic_mode = false;

    // init_table_t(&string_constants); // chapter 21 challenge

    advance();

    while (!match(TOKEN_EOF)) {
        declaration();
    }

    obj_function_t *function = end_compiler();
    return parser.had_error ? NULL : function;
}

void mark_compiler_roots(void)
{
    compiler_t *compiler = current;
    while (compiler != NULL) {
        mark_object((obj_t*)compiler->function);
        compiler = compiler->enclosing;
    }
}
