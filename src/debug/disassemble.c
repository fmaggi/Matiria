#include "disassemble.h"

#include "core/log.h"

u8* mtr_disassemble_instruction(u8* instruction, u32 offset) {
    MTR_PRINT("%04d ", offset);
#define READ(type) *((type*)instruction); instruction += sizeof(type)

    switch (*instruction++)
    {
    case MTR_OP_RETURN:
        MTR_LOG("RETURN");
        break;
    case MTR_OP_INT: {
        i64 constant = READ(i64);
        MTR_LOG("INT -> %li", constant);
        break;
    }

    case MTR_OP_FLOAT: {
        f64 constant = READ(f64);
        MTR_LOG("FLOAT -> %.2f", constant);
        break;
    }

    case MTR_OP_FALSE: {
        MTR_LOG("FALSE");
        break;
    }

    case MTR_OP_TRUE: {
        MTR_LOG("TRUE");
        break;
    }

    case MTR_OP_NIL:
        MTR_LOG("NIL");
        break;

    case MTR_OP_NOT:
        MTR_LOG("NOT");
        break;

    case MTR_OP_NEGATE_I:
        MTR_LOG("NEG");
        break;

    case MTR_OP_NEGATE_F:
        MTR_LOG("fNEG");
        break;

    case MTR_OP_ADD_I: MTR_LOG("ADD"); break;
    case MTR_OP_SUB_I: MTR_LOG("SUB"); break;
    case MTR_OP_MUL_I: MTR_LOG("MUL"); break;
    case MTR_OP_DIV_I: MTR_LOG("DIV"); break;

    case MTR_OP_ADD_F: MTR_LOG("fADD"); break;
    case MTR_OP_SUB_F: MTR_LOG("fSUB"); break;
    case MTR_OP_MUL_F: MTR_LOG("fMUL"); break;
    case MTR_OP_DIV_F: MTR_LOG("fDIV"); break;

    case MTR_OP_GET: {
        u16 index = READ(u16);
        MTR_LOG("GET at %u", index);
        break;
    }
    case  MTR_OP_SET: {
        u16 index = READ(u16);
        MTR_LOG("SET at %u", index);
        break;
    }

    case MTR_OP_JMP: {
        u16 to = READ(u16);
        MTR_LOG("JMP %u", to);
        break;
    }

    case MTR_OP_JMP_Z: {
        u16 to = READ(u16);
        MTR_LOG("ZJMP %u", to);
        break;
    }

    case MTR_OP_POP: {
        MTR_LOG("POP");
        break;
    }

    case MTR_OP_END_SCOPE: {
        u16 where = READ(u16);
        MTR_LOG("END_S %u", where);
        break;
    }
    default:
        break;
    }
    return instruction;
#undef READ
}

void mtr_disassemble(struct mtr_chunk chunk, const char* name) {
    MTR_LOG("====== %s =======", name);
    u8* instruction = chunk.bytecode;
    while (instruction != chunk.bytecode + chunk.size) {
        instruction = mtr_disassemble_instruction(instruction, instruction - chunk.bytecode);
    }
    MTR_LOG("\n");
}

void mtr_dump_stack(mtr_value* stack, mtr_value* top) {
    MTR_PRINT_DEBUG("[");
    while(stack != top) {
        MTR_PRINT_DEBUG("%lu,", stack->integer);
        stack++;
    }
    MTR_LOG("]");
}

void mtr_dump_chunk(struct mtr_chunk* chunk) {
    u8* ip = chunk->bytecode;
    while (ip < chunk->bytecode + chunk->size) {

        for (int i = 0; i < 8; ++i) {
            u32 offset = ip - chunk->bytecode;
            MTR_LOG("%04u %02x", offset, *ip++);
        }
        MTR_LOG("\n");
    }
}
