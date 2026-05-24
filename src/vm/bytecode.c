#include "bytecode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gc.h"
#include "token.h"

#define MAX_LOOP_DEPTH 64
#define MAX_BREAK_PATCHES 256

typedef struct {
    int break_patches[MAX_BREAK_PATCHES];
    int break_count;
    int continue_patches[MAX_BREAK_PATCHES];
    int continue_count;
    int continue_target;
    int has_for_cleanup;  // for-loops need extra stack cleanup before break
    int for_pop_count;    // number of extra pops needed for for-loop break
} LoopContext;

typedef struct {
    BytecodeChunk* chunk;
    BytecodeCompileMode mode;
    BytecodeBuildFunctionFn build_function;
    void* build_function_data;
    int allow_return;
    char* error;
    size_t error_size;
    LoopContext loops[MAX_LOOP_DEPTH];
    int loop_depth;
} BytecodeCompiler;

static void set_error(BytecodeCompiler* compiler, const char* message) {
    if (compiler->error != NULL && compiler->error_size > 0) {
        snprintf(compiler->error, compiler->error_size, "%s", message);
    }
}

static int ensure_byte_capacity(BytecodeChunk* chunk, int needed) {
    if (chunk->code_count + needed <= chunk->code_capacity) {
        return 1;
    }

    int new_capacity = chunk->code_capacity == 0 ? 64 : chunk->code_capacity * 2;
    while (new_capacity < chunk->code_count + needed) {
        new_capacity *= 2;
    }

    chunk->code = SAGE_REALLOC(chunk->code, (size_t)new_capacity);
    chunk->lines = SAGE_REALLOC(chunk->lines, sizeof(int) * (size_t)new_capacity);
    chunk->columns = SAGE_REALLOC(chunk->columns, sizeof(int) * (size_t)new_capacity);
    chunk->code_capacity = new_capacity;
    return 1;
}

static int ensure_constant_capacity(BytecodeChunk* chunk) {
    if (chunk->constant_count < chunk->constant_capacity) {
        return 1;
    }
    int new_capacity = chunk->constant_capacity == 0 ? 16 : chunk->constant_capacity * 2;
    chunk->constants = SAGE_REALLOC(chunk->constants, sizeof(Value) * (size_t)new_capacity);
    chunk->constant_capacity = new_capacity;
    return 1;
}

static int ensure_ast_stmt_capacity(BytecodeChunk* chunk) {
    if (chunk->ast_stmt_count < chunk->ast_stmt_capacity) {
        return 1;
    }
    int new_capacity = chunk->ast_stmt_capacity == 0 ? 8 : chunk->ast_stmt_capacity * 2;
    chunk->ast_stmts = SAGE_REALLOC(chunk->ast_stmts, sizeof(Stmt*) * (size_t)new_capacity);
    chunk->ast_stmt_capacity = new_capacity;
    return 1;
}

void bytecode_chunk_init(BytecodeChunk* chunk) {
    memset(chunk, 0, sizeof(*chunk));
}

void bytecode_chunk_free(BytecodeChunk* chunk) {
    free(chunk->code);
    free(chunk->lines);
    free(chunk->columns);
    free(chunk->constants);
    free(chunk->ast_stmts);
    memset(chunk, 0, sizeof(*chunk));
}

static int emit_byte(BytecodeCompiler* compiler, uint8_t byte, int line, int column) {
    BytecodeChunk* chunk = compiler->chunk;
    if (!ensure_byte_capacity(chunk, 1)) {
        set_error(compiler, "Out of memory while writing bytecode.");
        return 0;
    }
    chunk->code[chunk->code_count] = byte;
    chunk->lines[chunk->code_count] = line;
    chunk->columns[chunk->code_count] = column;
    chunk->code_count++;
    return 1;
}

static int emit_u16(BytecodeCompiler* compiler, uint16_t value, int line, int column) {
    return emit_byte(compiler, (uint8_t)((value >> 8) & 0xff), line, column) &&
           emit_byte(compiler, (uint8_t)(value & 0xff), line, column);
}

static int emit_u8(BytecodeCompiler* compiler, uint8_t value, int line, int column) {
    return emit_byte(compiler, value, line, column);
}

static int emit_op(BytecodeCompiler* compiler, BytecodeOp op, int line, int column) {
    return emit_byte(compiler, (uint8_t)op, line, column);
}

static int add_constant(BytecodeCompiler* compiler, Value value) {
    BytecodeChunk* chunk = compiler->chunk;
    if (!ensure_constant_capacity(chunk)) {
        set_error(compiler, "Out of memory while growing constant pool.");
        return -1;
    }
    chunk->constants[chunk->constant_count] = value;
    return chunk->constant_count++;
}

static int add_name_constant(BytecodeCompiler* compiler, const char* start, int length) {
    char* name = SAGE_ALLOC((size_t)length + 1);
    memcpy(name, start, (size_t)length);
    name[length] = '\0';
    int index = add_constant(compiler, val_string_take(name));
    return index;
}

static int add_ast_stmt(BytecodeCompiler* compiler, Stmt* stmt) {
    BytecodeChunk* chunk = compiler->chunk;
    if (!ensure_ast_stmt_capacity(chunk)) {
        set_error(compiler, "Out of memory while storing AST fallback statements.");
        return -1;
    }
    chunk->ast_stmts[chunk->ast_stmt_count] = stmt;
    return chunk->ast_stmt_count++;
}

static int emit_constant(BytecodeCompiler* compiler, Value value, int line, int column) {
    int index = add_constant(compiler, value);
    if (index < 0) return 0;
    if (index > 0xffff) {
        set_error(compiler, "Bytecode constant pool exceeded 65535 entries.");
        return 0;
    }
    return emit_op(compiler, BC_OP_CONSTANT, line, column) &&
           emit_u16(compiler, (uint16_t)index, line, column);
}

static int emit_name_op(BytecodeCompiler* compiler, BytecodeOp op, Token token) {
    int index = add_name_constant(compiler, token.start, token.length);
    if (index < 0) return 0;
    if (index > 0xffff) {
        set_error(compiler, "Bytecode name pool exceeded 65535 entries.");
        return 0;
    }
    return emit_op(compiler, op, token.line, token.column) &&
           emit_u16(compiler, (uint16_t)index, token.line, token.column);
}

static int emit_define_function(BytecodeCompiler* compiler, Token token, int function_index) {
    int name_index = add_name_constant(compiler, token.start, token.length);
    if (name_index < 0) return 0;
    if (name_index > 0xffff || function_index > 0xffff) {
        set_error(compiler, "Bytecode function table exceeded 65535 entries.");
        return 0;
    }
    return emit_op(compiler, BC_OP_DEFINE_FUNCTION, token.line, token.column) &&
           emit_u16(compiler, (uint16_t)name_index, token.line, token.column) &&
           emit_u16(compiler, (uint16_t)function_index, token.line, token.column);
}

static int emit_dup(BytecodeCompiler* compiler, uint8_t distance, int line, int column) {
    return emit_op(compiler, BC_OP_DUP, line, column) &&
           emit_u8(compiler, distance, line, column);
}

static int emit_ast_stmt(BytecodeCompiler* compiler, Stmt* stmt) {
    if (compiler->mode == BYTECODE_COMPILE_STRICT) {
        printf("DEBUG: Unsupported stmt type %d requires AST fallback\n", stmt->type);
        set_error(compiler,
                  "Statement requires AST fallback and cannot be emitted as a compiled VM artifact yet.");
        return 0;
    }

    int index = add_ast_stmt(compiler, stmt);
    if (index < 0) return 0;
    if (index > 0xffff) {
        set_error(compiler, "Bytecode AST fallback table exceeded 65535 entries.");
        return 0;
    }
    return emit_op(compiler, BC_OP_EXEC_AST_STMT, 0, 0) &&
           emit_u16(compiler, (uint16_t)index, 0, 0);
}

static int current_offset(BytecodeCompiler* compiler) {
    return compiler->chunk->code_count;
}

static int emit_jump(BytecodeCompiler* compiler, BytecodeOp op, int line, int column) {
    if (!emit_op(compiler, op, line, column)) return -1;
    int patch_location = current_offset(compiler);
    if (!emit_u16(compiler, 0, line, column)) return -1;
    return patch_location;
}

static int patch_jump(BytecodeCompiler* compiler, int patch_location, int target) {
    if (patch_location < 0 || patch_location + 1 >= compiler->chunk->code_count) {
        set_error(compiler, "Invalid jump patch location.");
        return 0;
    }
    compiler->chunk->code[patch_location] = (uint8_t)((target >> 8) & 0xff);
    compiler->chunk->code[patch_location + 1] = (uint8_t)(target & 0xff);
    return 1;
}

static int stmt_requires_ast_fallback(BytecodeCompiler* compiler, Stmt* stmt);
static int compile_stmt(BytecodeCompiler* compiler, Stmt* stmt, int want_result);
static int compile_expr(BytecodeCompiler* compiler, Expr* expr);

static int stmt_requires_ast_fallback(BytecodeCompiler* compiler, Stmt* stmt) {
    if (stmt == NULL) return 0;

    switch (stmt->type) {
        case STMT_BREAK:
        case STMT_CONTINUE:
            if (compiler->loop_depth <= 0) { printf("DEBUG: break/continue outside loop\n"); return 1; }
            return 0;
        case STMT_YIELD:
            return 1;
        case STMT_RETURN:
            if (!compiler->allow_return) { printf("DEBUG: STMT_RETURN not allowed\n"); return 1; }
            return 0;
        case STMT_BLOCK: {
            for (Stmt* current = stmt->as.block.statements; current != NULL; current = current->next) {
                if (stmt_requires_ast_fallback(compiler, current)) return 1;
            }
            return 0;
        }
        case STMT_IF:
            return stmt_requires_ast_fallback(compiler, stmt->as.if_stmt.then_branch) ||
                   stmt_requires_ast_fallback(compiler, stmt->as.if_stmt.else_branch);
        case STMT_WHILE: {
            compiler->loop_depth++;
            int res = stmt_requires_ast_fallback(compiler, stmt->as.while_stmt.body);
            compiler->loop_depth--;
            return res;
        }
        case STMT_FOR: {
            // Fall back to AST if iterable is a range expression or string
            Expr* it = stmt->as.for_stmt.iterable;
            if (it && it->type == EXPR_RANGE) return 1;
            // Also fall back if body needs AST
            compiler->loop_depth++;
            int res = stmt_requires_ast_fallback(compiler, stmt->as.for_stmt.body);
            compiler->loop_depth--;
            return res;
        }
        case STMT_TRY:
            return 1;
        case STMT_PROC:
        case STMT_CLASS:
            return 0;
        case STMT_ASYNC_PROC:
            return 1;
        case STMT_ANNOTATED_BLOCK:
            // Memory/safety blocks use AST interpreter for Phase 1
            // Full bytecode support comes in Phase 3
            return 0;  // body compiles normally; annotation semantics enforced by AST
        case STMT_SPAWN:
            return 0;  // spawn compiles normally — body runs in thread via interpreter
        case STMT_IMPORT:
            return 1;
        default:
            return 0;
    }
}

static int compile_short_circuit(BytecodeCompiler* compiler, BinaryExpr* binary) {
    if (!compile_expr(compiler, binary->left)) return 0;

    if (binary->op.type == TOKEN_OR) {
        int jump_false = emit_jump(compiler, BC_OP_JUMP_IF_FALSE, binary->op.line, binary->op.column);
        if (jump_false < 0) return 0;
        if (!emit_op(compiler, BC_OP_POP, binary->op.line, binary->op.column)) return 0;
        if (!emit_op(compiler, BC_OP_TRUE, binary->op.line, binary->op.column)) return 0;
        int end_jump = emit_jump(compiler, BC_OP_JUMP, binary->op.line, binary->op.column);
        if (end_jump < 0) return 0;
        if (!patch_jump(compiler, jump_false, current_offset(compiler))) return 0;
        if (!emit_op(compiler, BC_OP_POP, binary->op.line, binary->op.column)) return 0;
        if (!compile_expr(compiler, binary->right)) return 0;
        if (!emit_op(compiler, BC_OP_TRUTHY, binary->op.line, binary->op.column)) return 0;
        return patch_jump(compiler, end_jump, current_offset(compiler));
    }

    int jump_false = emit_jump(compiler, BC_OP_JUMP_IF_FALSE, binary->op.line, binary->op.column);
    if (jump_false < 0) return 0;
    if (!emit_op(compiler, BC_OP_POP, binary->op.line, binary->op.column)) return 0;
    if (!compile_expr(compiler, binary->right)) return 0;
    if (!emit_op(compiler, BC_OP_TRUTHY, binary->op.line, binary->op.column)) return 0;
    int end_jump = emit_jump(compiler, BC_OP_JUMP, binary->op.line, binary->op.column);
    if (end_jump < 0) return 0;
    if (!patch_jump(compiler, jump_false, current_offset(compiler))) return 0;
    if (!emit_op(compiler, BC_OP_POP, binary->op.line, binary->op.column)) return 0;
    if (!emit_op(compiler, BC_OP_FALSE, binary->op.line, binary->op.column)) return 0;
    return patch_jump(compiler, end_jump, current_offset(compiler));
}

static int compile_expr(BytecodeCompiler* compiler, Expr* expr) {
    if (expr == NULL) {
        return emit_op(compiler, BC_OP_NIL, 0, 0);
    }

    switch (expr->type) {
        case EXPR_NUMBER:
            return emit_constant(compiler, val_number(expr->as.number.value), 0, 0);
        case EXPR_INT:
            return emit_constant(compiler, val_int(expr->as.int_val.value), 0, 0);
        case EXPR_STRING:
            return emit_constant(compiler, val_string(expr->as.string.value), 0, 0);
        case EXPR_BOOL:
            return emit_op(compiler, expr->as.boolean.value ? BC_OP_TRUE : BC_OP_FALSE, 0, 0);
        case EXPR_NIL:
            return emit_op(compiler, BC_OP_NIL, 0, 0);
        case EXPR_VARIABLE:
            return emit_name_op(compiler, BC_OP_GET_GLOBAL, expr->as.variable.name);
        case EXPR_ARRAY:
            for (int i = 0; i < expr->as.array.count; i++) {
                if (!compile_expr(compiler, expr->as.array.elements[i])) return 0;
            }
            return emit_op(compiler, BC_OP_ARRAY, 0, 0) &&
                   emit_u16(compiler, (uint16_t)expr->as.array.count, 0, 0);
        case EXPR_TUPLE:
            for (int i = 0; i < expr->as.tuple.count; i++) {
                if (!compile_expr(compiler, expr->as.tuple.elements[i])) return 0;
            }
            return emit_op(compiler, BC_OP_TUPLE, 0, 0) &&
                   emit_u16(compiler, (uint16_t)expr->as.tuple.count, 0, 0);
        case EXPR_DICT:
            for (int i = 0; i < expr->as.dict.count; i++) {
                if (!emit_constant(compiler, val_string(expr->as.dict.keys[i]), 0, 0)) return 0;
                if (!compile_expr(compiler, expr->as.dict.values[i])) return 0;
            }
            return emit_op(compiler, BC_OP_DICT, 0, 0) &&
                   emit_u16(compiler, (uint16_t)expr->as.dict.count, 0, 0);
        case EXPR_INDEX:
            return compile_expr(compiler, expr->as.index.array) &&
                   compile_expr(compiler, expr->as.index.index) &&
                   emit_op(compiler, BC_OP_GET_INDEX, 0, 0);
        case EXPR_INDEX_SET:
            return compile_expr(compiler, expr->as.index_set.array) &&
                   compile_expr(compiler, expr->as.index_set.index) &&
                   compile_expr(compiler, expr->as.index_set.value) &&
                   emit_op(compiler, BC_OP_SET_INDEX, 0, 0);
        case EXPR_SLICE:
            if (!compile_expr(compiler, expr->as.slice.array)) return 0;
            if (expr->as.slice.start != NULL) {
                if (!compile_expr(compiler, expr->as.slice.start)) return 0;
            } else if (!emit_op(compiler, BC_OP_NIL, 0, 0)) {
                return 0;
            }
            if (expr->as.slice.end != NULL) {
                if (!compile_expr(compiler, expr->as.slice.end)) return 0;
            } else if (!emit_op(compiler, BC_OP_NIL, 0, 0)) {
                return 0;
            }
            return emit_op(compiler, BC_OP_SLICE, 0, 0);
        case EXPR_GET:
            return compile_expr(compiler, expr->as.get.object) &&
                   emit_name_op(compiler, BC_OP_GET_PROPERTY, expr->as.get.property);
        case EXPR_SET:
            if (expr->as.set.object == NULL) {
                return compile_expr(compiler, expr->as.set.value) &&
                       emit_name_op(compiler, BC_OP_SET_GLOBAL, expr->as.set.property);
            }
            return compile_expr(compiler, expr->as.set.object) &&
                   compile_expr(compiler, expr->as.set.value) &&
                   emit_name_op(compiler, BC_OP_SET_PROPERTY, expr->as.set.property);
        case EXPR_CALL: {
            if (expr->as.call.callee->type == EXPR_GET) {
                GetExpr* get = &expr->as.call.callee->as.get;
                if (!compile_expr(compiler, get->object)) return 0;
                for (int i = 0; i < expr->as.call.arg_count; i++) {
                    if (!compile_expr(compiler, expr->as.call.args[i])) return 0;
                }
                int name_index = add_name_constant(compiler, get->property.start, get->property.length);
                if (name_index < 0) return 0;
                if (name_index > 0xffff || expr->as.call.arg_count > 0xff) {
                    set_error(compiler, "Method call operand limit exceeded.");
                    return 0;
                }
                return emit_op(compiler, BC_OP_CALL_METHOD, get->property.line, get->property.column) &&
                       emit_u16(compiler, (uint16_t)name_index, get->property.line, get->property.column) &&
                       emit_u8(compiler, (uint8_t)expr->as.call.arg_count, get->property.line, get->property.column);
            }

            if (!compile_expr(compiler, expr->as.call.callee)) return 0;
            for (int i = 0; i < expr->as.call.arg_count; i++) {
                if (!compile_expr(compiler, expr->as.call.args[i])) return 0;
            }
            if (expr->as.call.arg_count > 0xff) {
                set_error(compiler, "Call argument count exceeded 255.");
                return 0;
            }
            return emit_op(compiler, BC_OP_CALL, 0, 0) &&
                   emit_u8(compiler, (uint8_t)expr->as.call.arg_count, 0, 0);
        }
        case EXPR_BINARY: {
            BinaryExpr* binary = &expr->as.binary;
            if (binary->op.type == TOKEN_OR || binary->op.type == TOKEN_AND) {
                return compile_short_circuit(compiler, binary);
            }

            // Constant folding: both operands are numeric literals
            // P1: handle int×int (→int), int×float or float×float (→float)
            if (binary->left && binary->right) {
                int l_is_int = (binary->left->type == EXPR_INT);
                int l_is_num = (binary->left->type == EXPR_NUMBER);
                int r_is_int = (binary->right->type == EXPR_INT);
                int r_is_num = (binary->right->type == EXPR_NUMBER);
                if ((l_is_int || l_is_num) && (r_is_int || r_is_num)) {
                    int both_int = l_is_int && r_is_int;
                    int64_t Li = l_is_int ? binary->left->as.int_val.value : (int64_t)binary->left->as.number.value;
                    int64_t Ri = r_is_int ? binary->right->as.int_val.value : (int64_t)binary->right->as.number.value;
                    double Ld = l_is_int ? (double)binary->left->as.int_val.value : binary->left->as.number.value;
                    double Rd = r_is_int ? (double)binary->right->as.int_val.value : binary->right->as.number.value;
                    int ln = binary->op.line, co = binary->op.column;
                    switch (binary->op.type) {
                        case TOKEN_PLUS:  return emit_constant(compiler, both_int ? val_int(Li+Ri) : val_number(Ld+Rd), ln, co);
                        case TOKEN_MINUS: return emit_constant(compiler, both_int ? val_int(Li-Ri) : val_number(Ld-Rd), ln, co);
                        case TOKEN_STAR:  return emit_constant(compiler, both_int ? val_int(Li*Ri) : val_number(Ld*Rd), ln, co);
                        case TOKEN_SLASH:
                            if (both_int) return emit_constant(compiler, Ri!=0 ? val_int(Li/Ri) : val_int(0), ln, co);
                            return emit_constant(compiler, Rd!=0.0 ? val_number(Ld/Rd) : val_number(0.0), ln, co);
                        case TOKEN_PERCENT:
                            if (both_int) return emit_constant(compiler, Ri!=0 ? val_int(Li%Ri) : val_int(0), ln, co);
                            return emit_constant(compiler, Rd!=0.0 ? val_number(fmod(Ld,Rd)) : val_number(0.0), ln, co);
                        case TOKEN_LT:   return emit_constant(compiler, val_bool(both_int ? Li<Ri : Ld<Rd), ln, co);
                        case TOKEN_GT:   return emit_constant(compiler, val_bool(both_int ? Li>Ri : Ld>Rd), ln, co);
                        case TOKEN_LTE:  return emit_constant(compiler, val_bool(both_int ? Li<=Ri : Ld<=Rd), ln, co);
                        case TOKEN_GTE:  return emit_constant(compiler, val_bool(both_int ? Li>=Ri : Ld>=Rd), ln, co);
                        case TOKEN_EQ:   return emit_constant(compiler, val_bool(both_int ? Li==Ri : Ld==Rd), ln, co);
                        case TOKEN_NEQ:  return emit_constant(compiler, val_bool(both_int ? Li!=Ri : Ld!=Rd), ln, co);
                        case TOKEN_LSHIFT: return emit_constant(compiler, val_int(Li << Ri), ln, co);
                        case TOKEN_RSHIFT: return emit_constant(compiler, val_int(Li >> Ri), ln, co);
                        case TOKEN_AMP:  return emit_constant(compiler, val_int(Li & Ri), ln, co);
                        case TOKEN_PIPE: return emit_constant(compiler, val_int(Li | Ri), ln, co);
                        case TOKEN_CARET:return emit_constant(compiler, val_int(Li ^ Ri), ln, co);
                        default: break;
                    }
                }
            }

            if (!compile_expr(compiler, binary->left)) return 0;

            if (binary->op.type == TOKEN_NOT) {
                return emit_op(compiler, BC_OP_NOT, binary->op.line, binary->op.column);
            }
            if (binary->op.type == TOKEN_TILDE) {
                return emit_op(compiler, BC_OP_BIT_NOT, binary->op.line, binary->op.column);
            }

            if (binary->right == NULL) {
                set_error(compiler, "Unsupported unary bytecode expression.");
                return 0;
            }

            if (!compile_expr(compiler, binary->right)) return 0;

            switch (binary->op.type) {
                case TOKEN_PLUS: return emit_op(compiler, BC_OP_ADD, binary->op.line, binary->op.column);
                case TOKEN_MINUS: return emit_op(compiler, BC_OP_SUB, binary->op.line, binary->op.column);
                case TOKEN_STAR: return emit_op(compiler, BC_OP_MUL, binary->op.line, binary->op.column);
                case TOKEN_SLASH: return emit_op(compiler, BC_OP_DIV, binary->op.line, binary->op.column);
                case TOKEN_PERCENT: return emit_op(compiler, BC_OP_MOD, binary->op.line, binary->op.column);
                case TOKEN_EQ: return emit_op(compiler, BC_OP_EQUAL, binary->op.line, binary->op.column);
                case TOKEN_NEQ: return emit_op(compiler, BC_OP_NOT_EQUAL, binary->op.line, binary->op.column);
                case TOKEN_GT: return emit_op(compiler, BC_OP_GREATER, binary->op.line, binary->op.column);
                case TOKEN_GTE: return emit_op(compiler, BC_OP_GREATER_EQUAL, binary->op.line, binary->op.column);
                case TOKEN_LT: return emit_op(compiler, BC_OP_LESS, binary->op.line, binary->op.column);
                case TOKEN_LTE: return emit_op(compiler, BC_OP_LESS_EQUAL, binary->op.line, binary->op.column);
                case TOKEN_AMP: return emit_op(compiler, BC_OP_BIT_AND, binary->op.line, binary->op.column);
                case TOKEN_PIPE: return emit_op(compiler, BC_OP_BIT_OR, binary->op.line, binary->op.column);
                case TOKEN_CARET: return emit_op(compiler, BC_OP_BIT_XOR, binary->op.line, binary->op.column);
                case TOKEN_LSHIFT: return emit_op(compiler, BC_OP_SHIFT_LEFT, binary->op.line, binary->op.column);
                case TOKEN_RSHIFT: return emit_op(compiler, BC_OP_SHIFT_RIGHT, binary->op.line, binary->op.column);
                default:
                    set_error(compiler, "Unsupported binary expression in bytecode mode.");
                    return 0;
            }
        }
        case EXPR_AWAIT: {
            // Compile the awaited expression (should produce a thread handle)
            if (!compile_expr(compiler, expr->as.await.expression)) return 0;
            if (!emit_op(compiler, BC_OP_AWAIT, 0, 0)) return 0;
            return 1;
        case EXPR_OPTCHAIN:
        case EXPR_NULLCOAL:
        case EXPR_FORCE_UNWRAP:
        case EXPR_PROPAGATE:
        case EXPR_COMPTIME:
        case EXPR_SUPER:
            // These fall back to the AST interpreter
            set_error(compiler, "Expression requires AST interpreter.");
            return 0;
        case EXPR_RANGE:
            // Range expressions compile as a call to __make_range native (AST handles via fallback)
            set_error(compiler, "EXPR_RANGE requires for-loop AST fallback.");
            return 0;
        case EXPR_INTERP:
            // String interpolation needs AST evaluation context — use AST fallback
            set_error(compiler, "EXPR_INTERP requires AST fallback.");
            return 0;
        }
    }

    set_error(compiler, "Unsupported expression in bytecode mode.");
    return 0;
}

static int push_loop(BytecodeCompiler* compiler, int continue_target, int is_for, int for_pop_count) {
    if (compiler->loop_depth >= MAX_LOOP_DEPTH) {
        set_error(compiler, "Loop nesting depth exceeded.");
        return 0;
    }
    LoopContext* loop = &compiler->loops[compiler->loop_depth++];
    loop->break_count = 0;
    loop->continue_count = 0;
    loop->continue_target = continue_target;
    loop->has_for_cleanup = is_for;
    loop->for_pop_count = for_pop_count;
    return 1;
}

static int pop_loop_and_patch_breaks(BytecodeCompiler* compiler) {
    if (compiler->loop_depth <= 0) {
        set_error(compiler, "No loop to pop.");
        return 0;
    }
    compiler->loop_depth--;
    LoopContext* loop = &compiler->loops[compiler->loop_depth];
    int target = current_offset(compiler);
    for (int i = 0; i < loop->break_count; i++) {
        if (!patch_jump(compiler, loop->break_patches[i], target)) return 0;
    }
    return 1;
}

static int compile_block(BytecodeCompiler* compiler, Stmt* stmt) {
    for (Stmt* current = stmt->as.block.statements; current != NULL; current = current->next) {
        if (!compile_stmt(compiler, current, 0)) return 0;
    }
    return 1;
}

static int compile_stmt(BytecodeCompiler* compiler, Stmt* stmt, int want_result) {
    if (stmt == NULL) {
        if (want_result) {
            return emit_op(compiler, BC_OP_NIL, 0, 0);
        }
        return 1;
    }

    if (stmt_requires_ast_fallback(compiler, stmt)) {
        if (!emit_ast_stmt(compiler, stmt)) return 0;
        if (!want_result) {
            return emit_op(compiler, BC_OP_POP, 0, 0);
        }
        return 1;
    }

    switch (stmt->type) {
        case STMT_PRINT:
            if (!compile_expr(compiler, stmt->as.print.expression)) break;
            if (!emit_op(compiler, BC_OP_PRINT, 0, 0)) return 0;
            if (want_result) return emit_op(compiler, BC_OP_NIL, 0, 0);
            return 1;
        case STMT_LET:
            if (stmt->as.let.initializer != NULL) {
                if (!compile_expr(compiler, stmt->as.let.initializer)) break;
            } else if (!emit_op(compiler, BC_OP_NIL, 0, 0)) {
                return 0;
            }
            if (!emit_name_op(compiler, BC_OP_DEFINE_GLOBAL, stmt->as.let.name)) return 0;
            if (want_result) return emit_op(compiler, BC_OP_NIL, 0, 0);
            return 1;
        case STMT_PROC: {
            int function_index = -1;
            if (compiler->build_function == NULL) break;
            if (!compiler->build_function(compiler->build_function_data, &stmt->as.proc,
                                          compiler->error, compiler->error_size, &function_index)) {
                return 0;
            }
            if (!emit_define_function(compiler, stmt->as.proc.name, function_index)) return 0;
            if (want_result) return emit_op(compiler, BC_OP_NIL, 0, 0);
            return 1;
        }
        case STMT_EXPRESSION:
            if (!compile_expr(compiler, stmt->as.expression)) break;
            if (!want_result) return emit_op(compiler, BC_OP_POP, 0, 0);
            return 1;
        case STMT_BLOCK:
            if (!compile_block(compiler, stmt)) break;
            if (want_result) return emit_op(compiler, BC_OP_NIL, 0, 0);
            return 1;
        case STMT_IF: {
            if (!compile_expr(compiler, stmt->as.if_stmt.condition)) break;
            int else_jump = emit_jump(compiler, BC_OP_JUMP_IF_FALSE, 0, 0);
            if (else_jump < 0) return 0;
            if (!emit_op(compiler, BC_OP_POP, 0, 0)) return 0;
            if (!compile_stmt(compiler, stmt->as.if_stmt.then_branch, 0)) return 0;
            int end_jump = emit_jump(compiler, BC_OP_JUMP, 0, 0);
            if (end_jump < 0) return 0;
            if (!patch_jump(compiler, else_jump, current_offset(compiler))) return 0;
            if (!emit_op(compiler, BC_OP_POP, 0, 0)) return 0;
            if (stmt->as.if_stmt.else_branch != NULL) {
                if (!compile_stmt(compiler, stmt->as.if_stmt.else_branch, 0)) return 0;
            }
            if (!patch_jump(compiler, end_jump, current_offset(compiler))) return 0;
            if (want_result) return emit_op(compiler, BC_OP_NIL, 0, 0);
            return 1;
        }
        case STMT_WHILE: {
            int loop_start = current_offset(compiler);
            if (!compile_expr(compiler, stmt->as.while_stmt.condition)) break;
            int exit_jump = emit_jump(compiler, BC_OP_JUMP_IF_FALSE, 0, 0);
            if (exit_jump < 0) return 0;
            if (!emit_op(compiler, BC_OP_POP, 0, 0)) return 0;
            if (!push_loop(compiler, loop_start, 0, 0)) return 0;
            if (!compile_stmt(compiler, stmt->as.while_stmt.body, 0)) {
                compiler->loop_depth--;
                return 0;
            }
            if (!emit_op(compiler, BC_OP_JUMP, 0, 0)) return 0;
            if (!emit_u16(compiler, (uint16_t)loop_start, 0, 0)) return 0;
            if (!pop_loop_and_patch_breaks(compiler)) return 0;
            if (!patch_jump(compiler, exit_jump, current_offset(compiler))) return 0;
            if (!emit_op(compiler, BC_OP_POP, 0, 0)) return 0;
            if (want_result) return emit_op(compiler, BC_OP_NIL, 0, 0);
            return 1;
        }
        case STMT_FOR: {
            Token loop_var = stmt->as.for_stmt.variable;
            Expr* iterable_expr = stmt->as.for_stmt.iterable;
            if (iterable_expr->type == EXPR_RANGE) return 0; // range: AST handles

            if (!compile_expr(compiler, stmt->as.for_stmt.iterable)) break;
            if (!emit_op(compiler, BC_OP_PUSH_ENV, loop_var.line, loop_var.column)) return 0;
            if (!emit_constant(compiler, val_int(0), loop_var.line, loop_var.column)) return 0;

            int loop_start = current_offset(compiler);

            if (!emit_dup(compiler, 0, loop_var.line, loop_var.column) ||
                !emit_dup(compiler, 2, loop_var.line, loop_var.column) ||
                !emit_op(compiler, BC_OP_ARRAY_LEN, loop_var.line, loop_var.column) ||
                !emit_op(compiler, BC_OP_LESS, loop_var.line, loop_var.column)) {
                return 0;
            }

            int exit_jump = emit_jump(compiler, BC_OP_JUMP_IF_FALSE, loop_var.line, loop_var.column);
            if (exit_jump < 0) return 0;
            if (!emit_op(compiler, BC_OP_POP, loop_var.line, loop_var.column)) return 0;

            if (!emit_dup(compiler, 1, loop_var.line, loop_var.column) ||
                !emit_dup(compiler, 1, loop_var.line, loop_var.column) ||
                !emit_op(compiler, BC_OP_GET_INDEX, loop_var.line, loop_var.column) ||
                !emit_name_op(compiler, BC_OP_DEFINE_GLOBAL, loop_var)) {
                return 0;
            }

            // continue_target points to the increment section
            int continue_target_placeholder = current_offset(compiler);
            // for-loop break needs: pop comparison result, pop index, pop array, pop_env
            if (!push_loop(compiler, continue_target_placeholder, 1, 3)) return 0;

            if (!compile_stmt(compiler, stmt->as.for_stmt.body, 0)) {
                compiler->loop_depth--;
                return 0;
            }

            // Patch continue target to the increment section (right here)
            {
                int inc_offset = current_offset(compiler);
                LoopContext* floop = &compiler->loops[compiler->loop_depth - 1];
                floop->continue_target = inc_offset;
                for (int ci = 0; ci < floop->continue_count; ci++) {
                    if (!patch_jump(compiler, floop->continue_patches[ci], inc_offset)) return 0;
                }
            }

            if (!emit_constant(compiler, val_int(1), loop_var.line, loop_var.column) ||
                !emit_op(compiler, BC_OP_ADD, loop_var.line, loop_var.column) ||
                !emit_op(compiler, BC_OP_JUMP, loop_var.line, loop_var.column) ||
                !emit_u16(compiler, (uint16_t)loop_start, loop_var.line, loop_var.column)) {
                compiler->loop_depth--;
                return 0;
            }

            if (!pop_loop_and_patch_breaks(compiler)) return 0;

            if (!patch_jump(compiler, exit_jump, current_offset(compiler)) ||
                !emit_op(compiler, BC_OP_POP, loop_var.line, loop_var.column) ||
                !emit_op(compiler, BC_OP_POP, loop_var.line, loop_var.column) ||
                !emit_op(compiler, BC_OP_POP, loop_var.line, loop_var.column) ||
                !emit_op(compiler, BC_OP_POP_ENV, loop_var.line, loop_var.column)) {
                return 0;
            }

            if (want_result) return emit_op(compiler, BC_OP_NIL, loop_var.line, loop_var.column);
            return 1;
        }
        case STMT_BREAK: {
            if (compiler->loop_depth <= 0) break;  // fall to AST fallback
            LoopContext* loop = &compiler->loops[compiler->loop_depth - 1];
            // For-loops need to clean up stack: pop index, pop array, and pop env
            if (loop->has_for_cleanup) {
                for (int i = 0; i < loop->for_pop_count; i++) {
                    if (!emit_op(compiler, BC_OP_POP, 0, 0)) return 0;
                }
                if (!emit_op(compiler, BC_OP_POP_ENV, 0, 0)) return 0;
            }
            if (loop->break_count >= MAX_BREAK_PATCHES) {
                set_error(compiler, "Too many break statements in loop.");
                return 0;
            }
            int jump_loc = emit_jump(compiler, BC_OP_JUMP, 0, 0);
            if (jump_loc < 0) return 0;
            loop->break_patches[loop->break_count++] = jump_loc;
            if (want_result) return emit_op(compiler, BC_OP_NIL, 0, 0);
            return 1;
        }
        case STMT_CONTINUE: {
            if (compiler->loop_depth <= 0) break;  // fall to AST fallback
            LoopContext* loop = &compiler->loops[compiler->loop_depth - 1];
            if (loop->has_for_cleanup) {
                // For-loops: increment target not yet known, use patch-list
                if (loop->continue_count >= MAX_BREAK_PATCHES) {
                    set_error(compiler, "Too many continue statements in loop.");
                    return 0;
                }
                int jump_loc = emit_jump(compiler, BC_OP_JUMP, 0, 0);
                if (jump_loc < 0) return 0;
                loop->continue_patches[loop->continue_count++] = jump_loc;
            } else {
                // While-loops: continue_target is loop_start, already known
                if (!emit_op(compiler, BC_OP_JUMP, 0, 0)) return 0;
                if (!emit_u16(compiler, (uint16_t)loop->continue_target, 0, 0)) return 0;
            }
            if (want_result) return emit_op(compiler, BC_OP_NIL, 0, 0);
            return 1;
        }
        case STMT_ANNOTATED_BLOCK: {
            // Compile the body normally — annotation is metadata for Phase 3
            if (stmt->as.annotated_block.statements) {
                return compile_stmt(compiler, stmt->as.annotated_block.statements, want_result);
            }
            if (want_result) return emit_op(compiler, BC_OP_NIL, 0, 0);
            return 1;
        }
        case STMT_SPAWN: {
            // Compile: create a proc wrapping the body, call thread_spawn_native with it
            // For now: emit AST_EXEC opcode for the whole spawn statement
            // The AST interpreter handles STMT_SPAWN natively
            // emit BC_OP_AST_EXEC is not available — use the fallback path:
            // emit nothing and let the fallback handle it
            // Actually: STMT_SPAWN compiles its body as a regular block (proc body not needed)
            // The spawn is handled by the interpreter's STMT_SPAWN case via AST fallback
            // Since we returned 0 from stmt_requires_ast_fallback, this will be
            // compiled and dispatched through the VM's STMT_SPAWN handler (which we need to add)
            // For now: the spawn block just executes synchronously in MossVM
            // Full async spawn in MossVM Phase 6
            if (stmt->as.spawn_stmt.body) {
                return compile_stmt(compiler, stmt->as.spawn_stmt.body, 0);
            }
            return 1;
        }
        case STMT_IMPORT: {
            // Fall through to AST fallback for now — module loading requires interpreter
            break;
        }
        case STMT_TRY: {
            // Fall through to AST fallback — exception handling requires handler stack
            break;
        }
        case STMT_RAISE: {
            // Fall through to AST fallback
            break;
        }
        case STMT_CLASS: {
            // Fall through to AST fallback — class definitions require interpreter
            break;
        }
        case STMT_RETURN:
            if (!compiler->allow_return) break;
            if (stmt->as.ret.value != NULL) {
                if (!compile_expr(compiler, stmt->as.ret.value)) return 0;
            } else if (!emit_op(compiler, BC_OP_NIL, 0, 0)) {
                return 0;
            }
            return emit_op(compiler, BC_OP_RETURN, 0, 0);
        default:
            break;
    }

    if (!emit_ast_stmt(compiler, stmt)) return 0;
    if (!want_result) {
        return emit_op(compiler, BC_OP_POP, 0, 0);
    }
    return 1;
}

int bytecode_compile_statement(BytecodeChunk* chunk, Stmt* stmt, char* error, size_t error_size) {
    return bytecode_compile_statement_mode(chunk, stmt, BYTECODE_COMPILE_HYBRID, error, error_size);
}

int bytecode_compile_statement_mode(BytecodeChunk* chunk, Stmt* stmt, BytecodeCompileMode mode,
                                    char* error, size_t error_size) {
    return bytecode_compile_statement_with_functions(chunk, stmt, mode, NULL, NULL, error, error_size);
}

int bytecode_compile_statement_with_functions(BytecodeChunk* chunk, Stmt* stmt, BytecodeCompileMode mode,
                                              BytecodeBuildFunctionFn build_function,
                                              void* build_function_data,
                                              char* error, size_t error_size) {
    gc_pin();
    BytecodeCompiler compiler;
    memset(&compiler, 0, sizeof(compiler));
    compiler.chunk = chunk;
    compiler.mode = mode;
    compiler.build_function = build_function;
    compiler.build_function_data = build_function_data;
    compiler.allow_return = 0;
    compiler.error = error;
    compiler.error_size = error_size;
    if (error != NULL && error_size > 0) {
        error[0] = '\0';
    }

    int success = compile_stmt(&compiler, stmt, 1);
    if (!success) {
        if (error != NULL && error[0] == '\0') {
            snprintf(error, error_size, "failed to compile statement");
        }
    } else {
        success = emit_op(&compiler, BC_OP_RETURN, 0, 0);
    }
    gc_unpin();
    return success;
}

int bytecode_compile_function_body(BytecodeChunk* chunk, Stmt* body,
                                   BytecodeBuildFunctionFn build_function,
                                   void* build_function_data,
                                   char* error, size_t error_size) {
    BytecodeCompiler compiler;
    memset(&compiler, 0, sizeof(compiler));
    compiler.chunk = chunk;
    compiler.mode = BYTECODE_COMPILE_STRICT;
    compiler.build_function = build_function;
    compiler.build_function_data = build_function_data;
    compiler.allow_return = 1;
    compiler.error = error;
    compiler.error_size = error_size;
    if (error != NULL && error_size > 0) {
        error[0] = '\0';
    }

    if (!compile_stmt(&compiler, body, 0)) {
        if (error != NULL && error_size > 0 && error[0] == '\0') {
            snprintf(error, error_size, "failed to compile function body");
        }
        return 0;
    }

    if (!emit_op(&compiler, BC_OP_NIL, 0, 0) ||
        !emit_op(&compiler, BC_OP_RETURN, 0, 0)) {
        return 0;
    }
    return 1;
}
