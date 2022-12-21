#ifndef clox_chunk_h
#define clox_chunk_h

#include "common.h"
#include "value.h"

typedef enum {
    OP_CONSTANT,
    OP_CONSTANT_LONG,
    OP_NIL,
    OP_TRUE,
    OP_FALSE,
    OP_EQUAL,
    OP_GREATER,
    OP_LESS,
    OP_ADD,
    OP_SUBTRACT,
    OP_MULTIPLY,
    OP_DIVIDE,
    OP_NOT,
    OP_NEGATE,
    OP_RETURN,
} op_code_t;

typedef struct {
    int offset;
    int line;
} line_info_t;

typedef struct {
    int count;
    int capacity;
    uint8_t *code;
    value_array_t constants;

    // line info
    int line_count;
    int line_capacity;
    line_info_t *lines;
} chunk_t;

void init_chunk(chunk_t *chunk);
void free_chunk(chunk_t *chunk);
void write_chunk(chunk_t *chunk, uint8_t byte, int line);
uint32_t add_constant(chunk_t *chunk, value_t value);
int get_line(chunk_t *chunk, int instruction);
void write_constant(chunk_t *chunk, value_t value, int line);

#endif