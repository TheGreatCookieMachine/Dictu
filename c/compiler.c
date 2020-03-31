#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "compiler.h"
#include "memory.h"
#include "vm.h"

#ifdef DEBUG_PRINT_CODE
#include "debug.h"
#endif

static Chunk *currentChunk(Compiler *compiler) {
    return &compiler->function->chunk;
}

static void errorAt(Parser *parser, Token *token, const char *message) {
    if (parser->panicMode) return;
    parser->panicMode = true;

    fprintf(stderr, "[line %d] Error", token->line);

    if (token->type == TOKEN_EOF) {
        fprintf(stderr, " at end");
    } else if (token->type == TOKEN_ERROR) {
        // Nothing.
    } else {
        fprintf(stderr, " at '%.*s'", token->length, token->start);
    }

    fprintf(stderr, ": %s\n", message);
    parser->hadError = true;
}

static void error(Parser *parser, const char *message) {
    errorAt(parser, &parser->previous, message);
}

static void errorAtCurrent(Parser *parser, const char *message) {
    errorAt(parser, &parser->current, message);
}

static void advance(Parser *parser) {
    parser->previous = parser->current;

    for (;;) {
        parser->current = scanToken();
        if (parser->current.type != TOKEN_ERROR) break;

        errorAtCurrent(parser, parser->current.start);
    }
}

static void consume(Compiler *compiler, TokenType type, const char *message) {
    if (compiler->parser->current.type == type) {
        advance(compiler->parser);
        return;
    }

    errorAtCurrent(compiler->parser, message);
}

static bool check(Compiler *compiler, TokenType type) {
    return compiler->parser->current.type == type;
}

static bool match(Compiler *compiler, TokenType type) {
    if (!check(compiler, type)) return false;
    advance(compiler->parser);
    return true;
}

static void emitByte(Compiler *compiler, uint8_t byte) {
    writeChunk(compiler->parser->vm, currentChunk(compiler), byte, compiler->parser->previous.line);
}

static void emitBytes(Compiler *compiler, uint8_t byte1, uint8_t byte2) {
    emitByte(compiler, byte1);
    emitByte(compiler, byte2);
}

static void emitLoop(Compiler *compiler, int loopStart) {
    emitByte(compiler, OP_LOOP);

    int offset = currentChunk(compiler)->count - loopStart + 2;
    if (offset > UINT16_MAX) error(compiler->parser, "Loop body too large.");

    emitByte(compiler, (offset >> 8) & 0xff);
    emitByte(compiler, offset & 0xff);
}

// Emits [instruction] followed by a placeholder for a jump offset. The
// placeholder can be patched by calling [jumpPatch]. Returns the index
// of the placeholder.
static int emitJump(Compiler *compiler, uint8_t instruction) {
    emitByte(compiler, instruction);
    emitByte(compiler, 0xff);
    emitByte(compiler, 0xff);
    return currentChunk(compiler)->count - 2;
}

static void emitReturn(Compiler *compiler) {
    // An initializer automatically returns "this".
    if (compiler->type == TYPE_INITIALIZER) {
        emitBytes(compiler, OP_GET_LOCAL, 0);
    } else {
        emitByte(compiler, OP_NIL);
    }

    emitByte(compiler, OP_RETURN);
}

static uint8_t makeConstant(Compiler *compiler, Value value) {
    int constant = addConstant(compiler->parser->vm, currentChunk(compiler), value);
    if (constant > UINT8_MAX) {
        error(compiler->parser, "Too many constants in one chunk.");
        return 0;
    }

    return (uint8_t) constant;
}

static void emitConstant(Compiler *compiler, Value value) {
    emitBytes(compiler, OP_CONSTANT, makeConstant(compiler, value));
}

// Replaces the placeholder argument for a previous CODE_JUMP or
// CODE_JUMP_IF instruction with an offset that jumps to the current
// end of bytecode.
static void patchJump(Compiler *compiler, int offset) {
    // -2 to adjust for the bytecode for the jump offset itself.
    int jump = currentChunk(compiler)->count - offset - 2;

    if (jump > UINT16_MAX) {
        error(compiler->parser, "Too much code to jump over.");
    }

    currentChunk(compiler)->code[offset] = (jump >> 8) & 0xff;
    currentChunk(compiler)->code[offset + 1] = jump & 0xff;
}

static void initCompiler(Parser *parser, Compiler *compiler, Compiler *parent, FunctionType type) {
    compiler->parser = parser;
    compiler->enclosing = parent;
    initTable(&compiler->stringConstants);
    compiler->function = NULL;
    compiler->class = NULL;
    compiler->loop = NULL;

    if (parent != NULL) {
        compiler->class = parent->class;
        compiler->loop = parent->loop;
    }

    compiler->type = type;
    compiler->localCount = 0;
    compiler->scopeDepth = 0;

    parser->vm->compiler = compiler;

    compiler->function = newFunction(parser->vm, type == TYPE_STATIC);

    switch (type) {
        case TYPE_INITIALIZER:
        case TYPE_METHOD:
        case TYPE_STATIC:
        case TYPE_FUNCTION: {
            compiler->function->name = copyString(
                    parser->vm,
                    parser->previous.start,
                    parser->previous.length
            );
            break;
        }
        case TYPE_TOP_LEVEL: {
            compiler->function->name = NULL;
            break;
        }
    }

    Local *local = &compiler->locals[compiler->localCount++];
    local->depth = compiler->scopeDepth;
    local->isUpvalue = false;
    if (type != TYPE_FUNCTION && type != TYPE_STATIC) {
        // In a method, it holds the receiver, "this".
        local->name.start = "this";
        local->name.length = 4;
    } else {
        // In a function, it holds the function, but cannot be referenced,
        // so has no name.
        local->name.start = "";
        local->name.length = 0;
    }
}

static ObjFunction *endCompiler(Compiler *compiler) {
    emitReturn(compiler);

    ObjFunction *function = compiler->function;
#ifdef DEBUG_PRINT_CODE
    if (!compiler->parser->hadError) {

      disassembleChunk(currentChunk(compiler),
          function->name != NULL ? function->name->chars : "<top>");
    }
#endif
    if (compiler->enclosing != NULL) {
        // Capture the upvalues in the new closure object.
        emitBytes(compiler->enclosing, OP_CLOSURE, makeConstant(compiler->enclosing, OBJ_VAL(function)));

        // Emit arguments for each upvalue to know whether to capture a local
        // or an upvalue.
        for (int i = 0; i < function->upvalueCount; i++) {
            emitByte(compiler->enclosing, compiler->upvalues[i].isLocal ? 1 : 0);
            emitByte(compiler->enclosing, compiler->upvalues[i].index);
        }
    }

    freeTable(compiler->parser->vm, &compiler->stringConstants);
    compiler->parser->vm->compiler = compiler->enclosing;
    return function;
}

static void beginScope(Compiler *compiler) {
    compiler->scopeDepth++;
}

static void endScope(Compiler *compiler) {
    compiler->scopeDepth--;

    while (compiler->localCount > 0 &&
           compiler->locals[compiler->localCount - 1].depth >
           compiler->scopeDepth) {

        if (compiler->locals[compiler->localCount - 1].isUpvalue) {
            emitByte(compiler, OP_CLOSE_UPVALUE);
        } else {
            emitByte(compiler, OP_POP);
        }
        compiler->localCount--;
    }
}

static void expression(Compiler *compiler);

static void statement(Compiler *compiler);

static void declaration(Compiler *compiler);

static ParseRule *getRule(TokenType type);

static void parsePrecedence(Compiler *compiler, Precedence precedence);

static uint8_t identifierConstant(Compiler *compiler, Token *name) {
    ObjString *string = copyString(compiler->parser->vm, name->start, name->length);
    Value indexValue;
    if (tableGet(&compiler->stringConstants, string, &indexValue)) {
        return (uint8_t) AS_NUMBER(indexValue);
    }

    uint8_t index = makeConstant(compiler, OBJ_VAL(string));
    tableSet(compiler->parser->vm, &compiler->stringConstants, string, NUMBER_VAL((double) index));
    return index;
}

static bool identifiersEqual(Token *a, Token *b) {
    if (a->length != b->length) return false;
    return memcmp(a->start, b->start, a->length) == 0;
}

static int resolveLocal(Compiler *compiler, Token *name, bool inFunction) {
    // Look it up in the local scopes. Look in reverse order so that the
    // most nested variable is found first and shadows outer ones.
    for (int i = compiler->localCount - 1; i >= 0; i--) {
        Local *local = &compiler->locals[i];
        if (identifiersEqual(name, &local->name)) {
            if (!inFunction && local->depth == -1) {
                error(compiler->parser, "Cannot read local variable in its own initializer.");
            }
            return i;
        }
    }

    return -1;
}

// Adds an upvalue to [compiler]'s function with the given properties.
// Does not add one if an upvalue for that variable is already in the
// list. Returns the index of the upvalue.
static int addUpvalue(Compiler *compiler, uint8_t index, bool isLocal) {
    // Look for an existing one.
    int upvalueCount = compiler->function->upvalueCount;
    for (int i = 0; i < upvalueCount; i++) {
        Upvalue *upvalue = &compiler->upvalues[i];
        if (upvalue->index == index && upvalue->isLocal == isLocal) {
            return i;
        }
    }

    // If we got here, it's a new upvalue.
    if (upvalueCount == UINT8_COUNT) {
        error(compiler->parser, "Too many closure variables in function.");
        return 0;
    }

    compiler->upvalues[upvalueCount].isLocal = isLocal;
    compiler->upvalues[upvalueCount].index = index;
    return compiler->function->upvalueCount++;
}

// Attempts to look up [name] in the functions enclosing the one being
// compiled by [compiler]. If found, it adds an upvalue for it to this
// compiler's list of upvalues (unless it's already in there) and
// returns its index. If not found, returns -1.
//
// If the name is found outside of the immediately enclosing function,
// this will flatten the closure and add upvalues to all of the
// intermediate functions so that it gets walked down to this one.
static int resolveUpvalue(Compiler *compiler, Token *name) {
    // If we are at the top level, we didn't find it.
    if (compiler->enclosing == NULL) return -1;

    // See if it's a local variable in the immediately enclosing function.
    int local = resolveLocal(compiler->enclosing, name, true);
    if (local != -1) {
        // Mark the local as an upvalue so we know to close it when it goes
        // out of scope.
        compiler->enclosing->locals[local].isUpvalue = true;
        return addUpvalue(compiler, (uint8_t) local, true);
    }

    // See if it's an upvalue in the immediately enclosing function. In
    // other words, if it's a local variable in a non-immediately
    // enclosing function. This "flattens" closures automatically: it
    // adds upvalues to all of the intermediate functions to get from the
    // function where a local is declared all the way into the possibly
    // deeply nested function that is closing over it.
    int upvalue = resolveUpvalue(compiler->enclosing, name);
    if (upvalue != -1) {
        return addUpvalue(compiler, (uint8_t) upvalue, false);
    }

    // If we got here, we walked all the way up the parent chain and
    // couldn't find it.
    return -1;
}

static void addLocal(Compiler *compiler, Token name) {
    if (compiler->localCount == UINT8_COUNT) {
        error(compiler->parser, "Too many local variables in function.");
        return;
    }

    Local *local = &compiler->locals[compiler->localCount];
    local->name = name;

    // The local is declared but not yet defined.
    local->depth = -1;
    local->isUpvalue = false;
    compiler->localCount++;
}

// Allocates a local slot for the value currently on the stack, if
// we're in a local scope.
static void declareVariable(Compiler *compiler) {
    // Global variables are implicitly declared.
    if (compiler->scopeDepth == 0) return;

    // See if a local variable with this name is already declared in this
    // scope.
    Token *name = &compiler->parser->previous;
    for (int i = compiler->localCount - 1; i >= 0; i--) {
        Local *local = &compiler->locals[i];
        if (local->depth != -1 && local->depth < compiler->scopeDepth) break;
        if (identifiersEqual(name, &local->name)) {
            error(compiler->parser, "Variable with this name already declared in this scope.");
        }
    }

    addLocal(compiler, *name);
}

static uint8_t parseVariable(Compiler *compiler, const char *errorMessage) {
    consume(compiler, TOKEN_IDENTIFIER, errorMessage);

    // If it's a global variable, create a string constant for it.
    if (compiler->scopeDepth == 0) {
        return identifierConstant(compiler, &compiler->parser->previous);
    }

    declareVariable(compiler);
    return 0;
}

static void defineVariable(Compiler *compiler, uint8_t global) {
    if (compiler->scopeDepth == 0) {
        emitBytes(compiler, OP_DEFINE_GLOBAL, global);
    } else {
        // Mark the local as defined now.
        compiler->locals[compiler->localCount - 1].depth = compiler->scopeDepth;
    }
}

static int argumentList(Compiler *compiler) {
    int argCount = 0;
    if (!check(compiler, TOKEN_RIGHT_PAREN)) {
        do {
            expression(compiler);
            argCount++;

            if (argCount > 255) {
                error(compiler->parser, "Cannot have more than 255 arguments.");
            }
        } while (match(compiler, TOKEN_COMMA));
    }

    consume(compiler, TOKEN_RIGHT_PAREN, "Expect ')' after arguments.");

    return argCount;
}

static void and_(Compiler *compiler, bool canAssign) {
    // left operand...
    // OP_JUMP_IF       ------.
    // OP_POP // left operand |
    // right operand...       |
    //   <--------------------'
    // ...

    // Short circuit if the left operand is false.
    int endJump = emitJump(compiler, OP_JUMP_IF_FALSE);

    // Compile the right operand.
    emitByte(compiler, OP_POP); // Left operand.
    parsePrecedence(compiler, PREC_AND);

    patchJump(compiler, endJump);
}

static void binary(Compiler *compiler, bool canAssign) {
    TokenType operatorType = compiler->parser->previous.type;

    ParseRule *rule = getRule(operatorType);
    parsePrecedence(compiler, (Precedence) (rule->precedence + 1));

    switch (operatorType) {
        case TOKEN_BANG_EQUAL:
            emitBytes(compiler, OP_EQUAL, OP_NOT);
            break;
        case TOKEN_EQUAL_EQUAL:
            emitByte(compiler, OP_EQUAL);
            break;
        case TOKEN_GREATER:
            emitByte(compiler, OP_GREATER);
            break;
        case TOKEN_GREATER_EQUAL:
            emitBytes(compiler, OP_LESS, OP_NOT);
            break;
        case TOKEN_LESS:
            emitByte(compiler, OP_LESS);
            break;
        case TOKEN_LESS_EQUAL:
            emitBytes(compiler, OP_GREATER, OP_NOT);
            break;
        case TOKEN_PLUS:
            emitByte(compiler, OP_ADD);
            break;
        case TOKEN_MINUS:
            emitBytes(compiler, OP_NEGATE, OP_ADD);
            break;
        case TOKEN_STAR:
            emitByte(compiler, OP_MULTIPLY);
            break;
        case TOKEN_STAR_STAR:
            emitByte(compiler, OP_POW);
            break;
        case TOKEN_SLASH:
            emitByte(compiler, OP_DIVIDE);
            break;
        case TOKEN_PERCENT:
            emitByte(compiler, OP_MOD);
            break;
        case TOKEN_AMPERSAND:
            emitByte(compiler, OP_BITWISE_AND);
            break;
        case TOKEN_CARET:
            emitByte(compiler, OP_BITWISE_XOR);
            break;
        case TOKEN_PIPE:
            emitByte(compiler, OP_BITWISE_OR);
            break;
        default:
            return;
    }
}

static void call(Compiler *compiler, bool canAssign) {
    int argCount = argumentList(compiler);
    emitBytes(compiler, OP_CALL, argCount);
}

static void dot(Compiler *compiler, bool canAssign) {
    consume(compiler, TOKEN_IDENTIFIER, "Expect property name after '.'.");
    uint8_t name = identifierConstant(compiler, &compiler->parser->previous);

    if (canAssign && match(compiler, TOKEN_EQUAL)) {
        expression(compiler);
        emitBytes(compiler, OP_SET_PROPERTY, name);
    } else if (match(compiler, TOKEN_LEFT_PAREN)) {
        int argCount = argumentList(compiler);
        emitBytes(compiler, OP_INVOKE, argCount);
        emitByte(compiler, name);
    } else if (canAssign && match(compiler, TOKEN_PLUS_EQUALS)) {
        emitBytes(compiler, OP_GET_PROPERTY_NO_POP, name);
        expression(compiler);
        emitByte(compiler, OP_ADD);
        emitBytes(compiler, OP_SET_PROPERTY, name);
    } else if (canAssign && match(compiler, TOKEN_MINUS_EQUALS)) {
        emitBytes(compiler, OP_GET_PROPERTY_NO_POP, name);
        expression(compiler);
        emitBytes(compiler, OP_NEGATE, OP_ADD);
        emitBytes(compiler, OP_SET_PROPERTY, name);
    } else if (canAssign && match(compiler, TOKEN_MULTIPLY_EQUALS)) {
        emitBytes(compiler, OP_GET_PROPERTY_NO_POP, name);
        expression(compiler);
        emitByte(compiler, OP_MULTIPLY);
        emitBytes(compiler, OP_SET_PROPERTY, name);
    } else if (canAssign && match(compiler, TOKEN_DIVIDE_EQUALS)) {
        emitBytes(compiler, OP_GET_PROPERTY_NO_POP, name);
        expression(compiler);
        emitByte(compiler, OP_DIVIDE);
        emitBytes(compiler, OP_SET_PROPERTY, name);
    } else if (canAssign && match(compiler, TOKEN_AMPERSAND_EQUALS)) {
        emitBytes(compiler, OP_GET_PROPERTY_NO_POP, name);
        expression(compiler);
        emitByte(compiler, OP_BITWISE_AND);
        emitBytes(compiler, OP_SET_PROPERTY, name);
    } else if (canAssign && match(compiler, TOKEN_CARET_EQUALS)) {
        emitBytes(compiler, OP_GET_PROPERTY_NO_POP, name);
        expression(compiler);
        emitByte(compiler, OP_BITWISE_XOR);
        emitBytes(compiler, OP_SET_PROPERTY, name);
    } else if (canAssign && match(compiler, TOKEN_PIPE_EQUALS)) {
        emitBytes(compiler, OP_GET_PROPERTY_NO_POP, name);
        expression(compiler);
        emitByte(compiler, OP_BITWISE_OR);
        emitBytes(compiler, OP_SET_PROPERTY, name);
    } else {
        emitBytes(compiler, OP_GET_PROPERTY, name);
    }
}

static void literal(Compiler *compiler, bool canAssign) {
    switch (compiler->parser->previous.type) {
        case TOKEN_FALSE:
            emitByte(compiler, OP_FALSE);
            break;
        case TOKEN_NIL:
            emitByte(compiler, OP_NIL);
            break;
        case TOKEN_TRUE:
            emitByte(compiler, OP_TRUE);
            break;
        default:
            return; // Unreachable.
    }
}

static void grouping(Compiler *compiler, bool canAssign) {
    expression(compiler);
    consume(compiler, TOKEN_RIGHT_PAREN, "Expect ')' after expression.");
}

static void number(Compiler *compiler, bool canAssign) {
    double value = strtod(compiler->parser->previous.start, NULL);
    emitConstant(compiler, NUMBER_VAL(value));
}

static void or_(Compiler *compiler, bool canAssign) {
    // left operand...
    // OP_JUMP_IF       ---.
    // OP_JUMP          ---+--.
    //   <-----------------'  |
    // OP_POP // left operand |
    // right operand...       |
    //   <--------------------'
    // ...

    // If the operand is *true* we want to keep it, so when it's false,
    // jump to the code to evaluate the right operand.
    int elseJump = emitJump(compiler, OP_JUMP_IF_FALSE);

    // If we get here, the operand is true, so jump to the end to keep it.
    int endJump = emitJump(compiler, OP_JUMP);

    // Compile the right operand.
    patchJump(compiler, elseJump);
    emitByte(compiler, OP_POP); // Left operand.

    parsePrecedence(compiler, PREC_OR);
    patchJump(compiler, endJump);
}

int parseString(char *string, int length) {
    for (int i = 0; i < length - 1; i++) {
        if (string[i] == '\\') {
            switch (string[i + 1]) {
                case 'n': {
                    string[i + 1] = '\n';
                    break;
                }
                case 't': {
                    string[i + 1] = '\t';
                    break;
                }
                case 'r': {
                    string[i + 1] = '\r';
                    break;
                }
                case 'v': {
                    string[i + 1] = '\v';
                    break;
                }
                case '\'':
                case '"': {
                    break;
                }
                default: {
                    continue;
                }
            }
            memmove(&string[i], &string[i + 1], length - i);
            length -= 1;
        }
    }

    return length;
}

static void string(Compiler *compiler, bool canAssign) {
    Parser *parser = compiler->parser;
    char *string = malloc(sizeof(char) * parser->previous.length - 1);
    memcpy(string, parser->previous.start + 1, parser->previous.length - 2);
    int length = parseString(string, parser->previous.length - 2);
    string[length] = '\0';

    emitConstant(compiler, OBJ_VAL(copyString(parser->vm, string, length)));
    free(string);
}

static void list(Compiler *compiler, bool canAssign) {
    emitByte(compiler, OP_NEW_LIST);

    do {
        if (check(compiler, TOKEN_RIGHT_BRACKET))
            break;

        expression(compiler);
        emitByte(compiler, OP_ADD_LIST);
    } while (match(compiler, TOKEN_COMMA));

    consume(compiler, TOKEN_RIGHT_BRACKET, "Expected closing ']'");
}

static void dict(Compiler *compiler, bool canAssign) {
    emitByte(compiler, OP_NEW_DICT);

    do {
        if (check(compiler, TOKEN_RIGHT_BRACE))
            break;

        expression(compiler);
        consume(compiler, TOKEN_COLON, "Expected ':'");
        expression(compiler);
        emitByte(compiler, OP_ADD_DICT);
    } while (match(compiler, TOKEN_COMMA));

    consume(compiler, TOKEN_RIGHT_BRACE, "Expected closing '}'");
}

static void subscript(Compiler *compiler, bool canAssign) {
    // slice with no initial index [1, 2, 3][:100]
    if (match(compiler, TOKEN_COLON)) {
        emitByte(compiler, OP_EMPTY);
        expression(compiler);
        emitByte(compiler, OP_SLICE);
        consume(compiler, TOKEN_RIGHT_BRACKET, "Expected closing ']'");
        return;
    }

    expression(compiler);

    if (match(compiler, TOKEN_COLON)) {
        // If we slice with no "ending" push EMPTY so we know
        // To go to the end of the iterable
        // i.e [1, 2, 3][1:]
        if (check(compiler, TOKEN_RIGHT_BRACKET)) {
            emitByte(compiler, OP_EMPTY);
        } else {
            expression(compiler);
        }
        emitByte(compiler, OP_SLICE);
        consume(compiler, TOKEN_RIGHT_BRACKET, "Expected closing ']'");
        return;
    }

    consume(compiler, TOKEN_RIGHT_BRACKET, "Expected closing ']'");

    if (canAssign && match(compiler, TOKEN_EQUAL)) {
        expression(compiler);
        emitByte(compiler, OP_SUBSCRIPT_ASSIGN);
    } else if (canAssign && match(compiler, TOKEN_PLUS_EQUALS)) {
        expression(compiler);
        emitBytes(compiler, OP_PUSH, OP_ADD);
        emitByte(compiler, OP_SUBSCRIPT_ASSIGN);
    } else if (canAssign && match(compiler, TOKEN_MINUS_EQUALS)) {
        expression(compiler);
        emitByte(compiler, OP_PUSH);
        emitBytes(compiler, OP_NEGATE, OP_ADD);
        emitByte(compiler, OP_SUBSCRIPT_ASSIGN);
    } else if (canAssign && match(compiler, TOKEN_MULTIPLY_EQUALS)) {
        expression(compiler);
        emitBytes(compiler, OP_PUSH, OP_MULTIPLY);
        emitByte(compiler, OP_SUBSCRIPT_ASSIGN);
    } else if (canAssign && match(compiler, TOKEN_DIVIDE_EQUALS)) {
        expression(compiler);
        emitBytes(compiler, OP_PUSH, OP_DIVIDE);
        emitByte(compiler, OP_SUBSCRIPT_ASSIGN);
    } else if (canAssign && match(compiler, TOKEN_AMPERSAND_EQUALS)) {
        expression(compiler);
        emitBytes(compiler, OP_PUSH, OP_BITWISE_AND);
        emitByte(compiler, OP_SUBSCRIPT_ASSIGN);
    } else if (canAssign && match(compiler, TOKEN_CARET_EQUALS)) {
        expression(compiler);
        emitBytes(compiler, OP_PUSH, OP_BITWISE_XOR);
        emitByte(compiler, OP_SUBSCRIPT_ASSIGN);
    } else if (canAssign && match(compiler, TOKEN_PIPE_EQUALS)) {
        expression(compiler);
        emitBytes(compiler, OP_PUSH, OP_BITWISE_OR);
        emitByte(compiler, OP_SUBSCRIPT_ASSIGN);
    } else {
        emitByte(compiler, OP_SUBSCRIPT);
    }
}

static void namedVariable(Compiler *compiler, Token name, bool canAssign) {
    uint8_t getOp, setOp;
    int arg = resolveLocal(compiler, &name, false);
    if (arg != -1) {
        getOp = OP_GET_LOCAL;
        setOp = OP_SET_LOCAL;
    } else if ((arg = resolveUpvalue(compiler, &name)) != -1) {
        getOp = OP_GET_UPVALUE;
        setOp = OP_SET_UPVALUE;
    } else {
        arg = identifierConstant(compiler, &name);
        getOp = OP_GET_GLOBAL;
        setOp = OP_SET_GLOBAL;
    }

    if (canAssign && match(compiler, TOKEN_EQUAL)) {
        expression(compiler);
        emitBytes(compiler, setOp, (uint8_t) arg);
    } else if (canAssign && match(compiler, TOKEN_PLUS_EQUALS)) {
        namedVariable(compiler, name, false);
        expression(compiler);
        emitByte(compiler, OP_ADD);
        emitBytes(compiler, setOp, (uint8_t) arg);
    } else if (canAssign && match(compiler, TOKEN_MINUS_EQUALS)) {
        namedVariable(compiler, name, false);
        expression(compiler);
        emitBytes(compiler, OP_NEGATE, OP_ADD);
        emitBytes(compiler, setOp, (uint8_t) arg);
    } else if (canAssign && match(compiler, TOKEN_MULTIPLY_EQUALS)) {
        namedVariable(compiler, name, false);
        expression(compiler);
        emitByte(compiler, OP_MULTIPLY);
        emitBytes(compiler, setOp, (uint8_t) arg);
    } else if (canAssign && match(compiler, TOKEN_DIVIDE_EQUALS)) {
        namedVariable(compiler, name, false);
        expression(compiler);
        emitByte(compiler, OP_DIVIDE);
        emitBytes(compiler, setOp, (uint8_t) arg);
    } else if (canAssign && match(compiler, TOKEN_AMPERSAND_EQUALS)) {
        namedVariable(compiler, name, false);
        expression(compiler);
        emitByte(compiler, OP_BITWISE_AND);
        emitBytes(compiler, setOp, (uint8_t) arg);
    } else if (canAssign && match(compiler, TOKEN_CARET_EQUALS)) {
        namedVariable(compiler, name, false);
        expression(compiler);
        emitByte(compiler, OP_BITWISE_XOR);
        emitBytes(compiler, setOp, (uint8_t) arg);
    } else if (canAssign && match(compiler, TOKEN_PIPE_EQUALS)) {
        namedVariable(compiler, name, false);
        expression(compiler);
        emitByte(compiler, OP_BITWISE_OR);
        emitBytes(compiler, setOp, (uint8_t) arg);
    } else {
        emitBytes(compiler, getOp, (uint8_t) arg);
    }
}

static void variable(Compiler *compiler, bool canAssign) {
    namedVariable(compiler, compiler->parser->previous, canAssign);
}

static Token syntheticToken(const char *text) {
    Token token;
    token.start = text;
    token.length = (int) strlen(text);
    return token;
}

static void pushSuperclass(Compiler *compiler) {
    if (compiler->class == NULL) return;
    namedVariable(compiler, syntheticToken("super"), false);
}

static void super_(Compiler *compiler, bool canAssign) {
    if (compiler->class == NULL) {
        error(compiler->parser, "Cannot utilise 'super' outside of a class.");
    } else if (!compiler->class->hasSuperclass) {
        error(compiler->parser, "Cannot utilise 'super' in a class with no superclass.");
    }

    consume(compiler, TOKEN_DOT, "Expect '.' after 'super'.");
    consume(compiler, TOKEN_IDENTIFIER, "Expect superclass method name.");
    uint8_t name = identifierConstant(compiler, &compiler->parser->previous);

    // Push the receiver.
    namedVariable(compiler, syntheticToken("this"), false);

    if (match(compiler, TOKEN_LEFT_PAREN)) {
        int argCount = argumentList(compiler);

        pushSuperclass(compiler);
        emitBytes(compiler, OP_SUPER, argCount);
        emitByte(compiler, name);
    } else {
        pushSuperclass(compiler);
        emitBytes(compiler, OP_GET_SUPER, name);
    }
}

static void this_(Compiler *compiler, bool canAssign) {
    if (compiler->class == NULL) {
        error(compiler->parser, "Cannot utilise 'this' outside of a class.");
    } else if (compiler->class->staticMethod) {
        error(compiler->parser, "Cannot utilise 'this' inside a static method.");
    } else {
        variable(compiler, false);
    }
}

static void static_(Compiler *compiler, bool canAssign) {
    if (compiler->class == NULL) {
        error(compiler->parser, "Cannot utilise 'static' outside of a class.");
    }
}

static void useStatement(Compiler *compiler) {
    if (compiler->class == NULL) {
        error(compiler->parser, "Cannot utilise 'use' outside of a class.");
    }

    do {
        consume(compiler, TOKEN_IDENTIFIER, "Expect trait name after use statement.");
        namedVariable(compiler, compiler->parser->previous, false);
        emitByte(compiler, OP_USE);
    } while (match(compiler, TOKEN_COMMA));

    consume(compiler, TOKEN_SEMICOLON, "Expect ';' after use statement.");
}

static void unary(Compiler *compiler, bool canAssign) {
    TokenType operatorType = compiler->parser->previous.type;

    parsePrecedence(compiler, PREC_UNARY);

    switch (operatorType) {
        case TOKEN_BANG:
            emitByte(compiler, OP_NOT);
            break;
        case TOKEN_MINUS:
            emitByte(compiler, OP_NEGATE);
            break;
        default:
            return;
    }
}

static void prefix(Compiler *compiler, bool canAssign) {
    TokenType operatorType = compiler->parser->previous.type;
    Token cur = compiler->parser->current;
    consume(compiler, TOKEN_IDENTIFIER, "Expected variable");
    namedVariable(compiler, compiler->parser->previous, true);

    int arg;
    bool instance = false;

    if (match(compiler, TOKEN_DOT)) {
        consume(compiler, TOKEN_IDENTIFIER, "Expect property name after '.'.");
        arg = identifierConstant(compiler, &compiler->parser->previous);
        emitBytes(compiler, OP_GET_PROPERTY_NO_POP, arg);
        instance = true;
    }

    switch (operatorType) {
        case TOKEN_PLUS_PLUS: {
            emitByte(compiler, OP_INCREMENT);
            break;
        }
        case TOKEN_MINUS_MINUS:
            emitByte(compiler, OP_DECREMENT);
            break;
        default:
            return;
    }

    if (instance) {
        emitBytes(compiler, OP_SET_PROPERTY, arg);
    } else {
        uint8_t setOp;
        arg = resolveLocal(compiler, &cur, false);
        if (arg != -1) {
            setOp = OP_SET_LOCAL;
        } else if ((arg = resolveUpvalue(compiler, &cur)) != -1) {
            setOp = OP_SET_UPVALUE;
        } else {
            arg = identifierConstant(compiler, &cur);
            setOp = OP_SET_GLOBAL;
        }

        emitBytes(compiler, setOp, (uint8_t) arg);
    }
}

ParseRule rules[] = {
        {grouping, call,      PREC_CALL},               // TOKEN_LEFT_PAREN
        {NULL,     NULL,      PREC_NONE},               // TOKEN_RIGHT_PAREN
        {dict,     NULL,      PREC_NONE},               // TOKEN_LEFT_BRACE [big]
        {NULL,     NULL,      PREC_NONE},               // TOKEN_RIGHT_BRACE
        {list,     subscript, PREC_CALL},               // TOKEN_LEFT_BRACKET
        {NULL,     NULL,      PREC_NONE},               // TOKEN_RIGHT_BRACKET
        {NULL,     NULL,      PREC_NONE},               // TOKEN_COMMA
        {NULL,     dot,       PREC_CALL},               // TOKEN_DOT
        {unary,    binary,    PREC_TERM},               // TOKEN_MINUS
        {NULL,     binary,    PREC_TERM},               // TOKEN_PLUS
        {prefix,   NULL,      PREC_NONE},               // TOKEN_PLUS_PLUS
        {prefix,   NULL,      PREC_NONE},               // TOKEN_MINUS_MINUS
        {NULL,     NULL,      PREC_NONE},               // TOKEN_PLUS_EQUALS
        {NULL,     NULL,      PREC_NONE},               // TOKEN_MINUS_EQUALS
        {NULL,     NULL,      PREC_NONE},               // TOKEN_MULTIPLY_EQUALS
        {NULL,     NULL,      PREC_NONE},               // TOKEN_DIVIDE_EQUALS
        {NULL,     NULL,      PREC_NONE},               // TOKEN_SEMICOLON
        {NULL,     NULL,      PREC_NONE},               // TOKEN_COLON
        {NULL,     binary,    PREC_FACTOR},             // TOKEN_SLASH
        {NULL,     binary,    PREC_FACTOR},             // TOKEN_STAR
        {NULL,     binary,    PREC_INDICES},            // TOKEN_STAR_STAR
        {NULL,     binary,    PREC_FACTOR},             // TOKEN_PERCENT
        {NULL,     binary,    PREC_BITWISE_AND},        // TOKEN_AMPERSAND
        {NULL,     binary,    PREC_BITWISE_XOR},        // TOKEN_CARET
        {NULL,     binary,    PREC_BITWISE_OR},         // TOKEN_PIPE
        {NULL,     NULL,      PREC_NONE},               // TOKEN_AMPERSAND_EQUALS
        {NULL,     NULL,      PREC_NONE},               // TOKEN_CARET_EQUALS
        {NULL,     NULL,      PREC_NONE},               // TOKEN_PIPE_EQUALS
        {unary,    NULL,      PREC_NONE},               // TOKEN_BANG
        {NULL,     binary,    PREC_EQUALITY},           // TOKEN_BANG_EQUAL
        {NULL,     NULL,      PREC_NONE},               // TOKEN_EQUAL
        {NULL,     binary,    PREC_EQUALITY},           // TOKEN_EQUAL_EQUAL
        {NULL,     binary,    PREC_COMPARISON},         // TOKEN_GREATER
        {NULL,     binary,    PREC_COMPARISON},         // TOKEN_GREATER_EQUAL
        {NULL,     binary,    PREC_COMPARISON},         // TOKEN_LESS
        {NULL,     binary,    PREC_COMPARISON},         // TOKEN_LESS_EQUAL
        {variable, NULL,      PREC_NONE},               // TOKEN_IDENTIFIER
        {string,   NULL,      PREC_NONE},               // TOKEN_STRING
        {number,   NULL,      PREC_NONE},               // TOKEN_NUMBER
        {NULL,     NULL,      PREC_NONE},               // TOKEN_CLASS
        {NULL,     NULL,      PREC_NONE},               // TOKEN_TRAIT
        {NULL,     NULL,      PREC_NONE},               // TOKEN_USE
        {static_,  NULL,      PREC_NONE},               // TOKEN_STATIC
        {this_,    NULL,      PREC_NONE},               // TOKEN_THIS
        {super_,   NULL,      PREC_NONE},               // TOKEN_SUPER
        {NULL,     NULL,      PREC_NONE},               // TOKEN_DEF
        {NULL,     NULL,      PREC_NONE},               // TOKEN_IF
        {NULL,     and_,      PREC_AND},                // TOKEN_AND
        {NULL,     NULL,      PREC_NONE},               // TOKEN_ELSE
        {NULL,     or_,       PREC_OR},                 // TOKEN_OR
        {NULL,     NULL,      PREC_NONE},               // TOKEN_VAR
        {literal,  NULL,      PREC_NONE},               // TOKEN_TRUE
        {literal,  NULL,      PREC_NONE},               // TOKEN_FALSE
        {literal,  NULL,      PREC_NONE},               // TOKEN_NIL
        {NULL,     NULL,      PREC_NONE},               // TOKEN_FOR
        {NULL,     NULL,      PREC_NONE},               // TOKEN_WHILE
        {NULL,     NULL,      PREC_NONE},               // TOKEN_BREAK
        {NULL,     NULL,      PREC_NONE},               // TOKEN_RETURN
        {NULL,     NULL,      PREC_NONE},               // TOKEN_CONTINUE
        {NULL,     NULL,      PREC_NONE},               // TOKEN_WITH
        {NULL,     NULL,      PREC_NONE},               // TOKEN_EOF
        {NULL,     NULL,      PREC_NONE},               // TOKEN_IMPORT
        {NULL,     NULL,      PREC_NONE},               // TOKEN_ERROR
};

static void parsePrecedence(Compiler *compiler, Precedence precedence) {
    Parser *parser = compiler->parser;
    advance(parser);
    ParseFn prefixRule = getRule(parser->previous.type)->prefix;
    if (prefixRule == NULL) {
        error(parser, "Expect expression.");
        return;
    }

    bool canAssign = precedence <= PREC_ASSIGNMENT;
    prefixRule(compiler, canAssign);

    while (precedence <= getRule(parser->current.type)->precedence) {
        advance(parser);
        ParseFn infixRule = getRule(parser->previous.type)->infix;
        infixRule(compiler, canAssign);
    }

    if (canAssign && match(compiler, TOKEN_EQUAL)) {
        // If we get here, we didn't parse the "=" even though we could
        // have, so the LHS must not be a valid lvalue.
        error(parser, "Invalid assignment target.");
    }
}

static ParseRule *getRule(TokenType type) {
    return &rules[type];
}

void expression(Compiler *compiler) {
    parsePrecedence(compiler, PREC_ASSIGNMENT);
}

static void block(Compiler *compiler) {
    while (!check(compiler, TOKEN_RIGHT_BRACE) && !check(compiler, TOKEN_EOF)) {
        declaration(compiler);
    }

    consume(compiler, TOKEN_RIGHT_BRACE, "Expect '}' after block.");
}

static void function(Compiler *compiler, FunctionType type) {
    Compiler fnCompiler;
    initCompiler(compiler->parser, &fnCompiler, compiler, type);
    beginScope(&fnCompiler);

    // Compile the parameter list.
    consume(&fnCompiler, TOKEN_LEFT_PAREN, "Expect '(' after function name.");

    if (!check(&fnCompiler, TOKEN_RIGHT_PAREN)) {
        bool optional = false;
        do {
            uint8_t paramConstant = parseVariable(&fnCompiler, "Expect parameter name.");
            defineVariable(&fnCompiler, paramConstant);

            if (match(&fnCompiler, TOKEN_EQUAL)) {
                fnCompiler.function->arityOptional++;
                optional = true;
                expression(&fnCompiler);
            } else {
                fnCompiler.function->arity++;

                if (optional) {
                    error(fnCompiler.parser, "Cannot have non-optional parameter after optional.");
                }
            }

            if (fnCompiler.function->arity + fnCompiler.function->arityOptional > 255) {
                error(fnCompiler.parser, "Cannot have more than 255 parameters.");
            }
        } while (match(&fnCompiler, TOKEN_COMMA));

        if (fnCompiler.function->arityOptional > 0) {
            emitByte(&fnCompiler, OP_DEFINE_OPTIONAL);
        }
    }

    consume(&fnCompiler, TOKEN_RIGHT_PAREN, "Expect ')' after parameters.");

    // The body.
    consume(&fnCompiler, TOKEN_LEFT_BRACE, "Expect '{' before function body.");

    block(&fnCompiler);

    /**
     * No need to explicitly reduce the scope here as endCompiler does
     * it for us.
     **/
    endCompiler(&fnCompiler);
}

static void method(Compiler *compiler, bool trait) {
    FunctionType type;

    if (check(compiler, TOKEN_STATIC)) {
        type = TYPE_STATIC;
        consume(compiler, TOKEN_STATIC, "Expect static.");
        compiler->class->staticMethod = true;
    } else {
        type = TYPE_METHOD;
        compiler->class->staticMethod = false;
    }

    consume(compiler, TOKEN_IDENTIFIER, "Expect method name.");
    uint8_t constant = identifierConstant(compiler, &compiler->parser->previous);

    // If the method is named "init", it's an initializer.
    if (compiler->parser->previous.length == 4 &&
        memcmp(compiler->parser->previous.start, "init", 4) == 0) {
        type = TYPE_INITIALIZER;
    }

    function(compiler, type);

    if (trait) {
        emitBytes(compiler, OP_TRAIT_METHOD, constant);
    } else {
        emitBytes(compiler, OP_METHOD, constant);
    }
}

static void classDeclaration(Compiler *compiler) {
    consume(compiler, TOKEN_IDENTIFIER, "Expect class name.");
    uint8_t nameConstant = identifierConstant(compiler, &compiler->parser->previous);
    declareVariable(compiler);

    ClassCompiler classCompiler;
    classCompiler.name = compiler->parser->previous;
    classCompiler.hasSuperclass = false;
    classCompiler.enclosing = compiler->class;
    classCompiler.staticMethod = false;
    compiler->class = &classCompiler;

    if (match(compiler, TOKEN_LESS)) {
        consume(compiler, TOKEN_IDENTIFIER, "Expect superclass name.");
        classCompiler.hasSuperclass = true;

        beginScope(compiler);

        // Store the superclass in a local variable named "super".
        variable(compiler, false);
        addLocal(compiler, syntheticToken("super"));

        emitBytes(compiler, OP_SUBCLASS, nameConstant);
    } else {
        emitBytes(compiler, OP_CLASS, nameConstant);
    }

    consume(compiler, TOKEN_LEFT_BRACE, "Expect '{' before class body.");

    while (!check(compiler, TOKEN_RIGHT_BRACE) && !check(compiler, TOKEN_EOF)) {
        if (match(compiler, TOKEN_USE)) {
            useStatement(compiler);
        } else {
            method(compiler, false);
        }
    }
    consume(compiler, TOKEN_RIGHT_BRACE, "Expect '}' after class body.");

    if (classCompiler.hasSuperclass) {
        endScope(compiler);
    }

    defineVariable(compiler, nameConstant);

    compiler->class = compiler->class->enclosing;
}

static void traitDeclaration(Compiler *compiler) {
    consume(compiler, TOKEN_IDENTIFIER, "Expect trait name.");
    uint8_t nameConstant = identifierConstant(compiler, &compiler->parser->previous);
    declareVariable(compiler);

    ClassCompiler classCompiler;
    classCompiler.name = compiler->parser->previous;
    classCompiler.hasSuperclass = false;
    classCompiler.enclosing = compiler->class;
    classCompiler.staticMethod = false;
    compiler->class = &classCompiler;

    emitBytes(compiler, OP_TRAIT, nameConstant);

    consume(compiler, TOKEN_LEFT_BRACE, "Expect '{' before trait body.");
    while (!check(compiler, TOKEN_RIGHT_BRACE) && !check(compiler, TOKEN_EOF)) {
        method(compiler, true);
    }
    consume(compiler, TOKEN_RIGHT_BRACE, "Expect '}' after trait body.");

    defineVariable(compiler, nameConstant);

    compiler->class = compiler->class->enclosing;
}

static void funDeclaration(Compiler *compiler) {
    uint8_t global = parseVariable(compiler, "Expect function name.");
    function(compiler, TYPE_FUNCTION);
    defineVariable(compiler, global);
}

static void varDeclaration(Compiler *compiler) {
    do {
        uint8_t global = parseVariable(compiler, "Expect variable name.");

        if (match(compiler, TOKEN_EQUAL)) {
            // Compile the initializer.
            expression(compiler);
        } else {
            // Default to nil.
            emitByte(compiler, OP_NIL);
        }

        defineVariable(compiler, global);
    } while (match(compiler, TOKEN_COMMA));

    consume(compiler, TOKEN_SEMICOLON, "Expect ';' after variable declaration.");
}

static void expressionStatement(Compiler *compiler) {
    expression(compiler);
    consume(compiler, TOKEN_SEMICOLON, "Expect ';' after expression.");
    if (compiler->parser->vm->repl) {
        emitByte(compiler, OP_POP_REPL);
    } else {
        emitByte(compiler, OP_POP);
    }
}

static void endLoop(Compiler *compiler) {
    if (compiler->loop->end != -1) {
        patchJump(compiler, compiler->loop->end);
        emitByte(compiler, OP_POP); // Condition.
    }

    int i = compiler->loop->body;
    while (i < compiler->function->chunk.count) {
        if (compiler->function->chunk.code[i] == OP_BREAK) {
            compiler->function->chunk.code[i] = OP_JUMP;
            patchJump(compiler, i + 1);
            i += 3;
        } else {
            i++;
        }
    }

    compiler->loop = compiler->loop->enclosing;
}

static void forStatement(Compiler *compiler) {
    // for (var i = 0; i < 10; i = i + 1) print i;
    //
    //   var i = 0;
    // start:                      <--.
    //   if (i < 10) goto exit;  --.  |
    //   goto body;  -----------.  |  |
    // increment:            <--+--+--+--.
    //   i = i + 1;             |  |  |  |
    //   goto start;  ----------+--+--'  |
    // body:                 <--'  |     |
    //   print i;                  |     |
    //   goto increment;  ---------+-----'
    // exit:                    <--'

    // Create a scope for the loop variable.
    beginScope(compiler);

    // The initialization clause.
    consume(compiler, TOKEN_LEFT_PAREN, "Expect '(' after 'for'.");
    if (match(compiler, TOKEN_VAR)) {
        varDeclaration(compiler);
    } else if (match(compiler, TOKEN_SEMICOLON)) {
        // No initializer.
    } else {
        expressionStatement(compiler);
    }

    Loop loop;
    loop.start = currentChunk(compiler)->count;
    loop.scopeDepth = compiler->scopeDepth;
    loop.enclosing = compiler->loop;
    compiler->loop = &loop;

    // The exit condition.
    compiler->loop->end = -1;

    if (!match(compiler, TOKEN_SEMICOLON)) {
        expression(compiler);
        consume(compiler, TOKEN_SEMICOLON, "Expect ';' after loop condition.");

        // Jump out of the loop if the condition is false.
        compiler->loop->end = emitJump(compiler, OP_JUMP_IF_FALSE);
        emitByte(compiler, OP_POP); // Condition.
    }

    // Increment step.
    if (!match(compiler, TOKEN_RIGHT_PAREN)) {
        // We don't want to execute the increment before the body, so jump
        // over it.
        int bodyJump = emitJump(compiler, OP_JUMP);

        int incrementStart = currentChunk(compiler)->count;
        expression(compiler);
        emitByte(compiler, OP_POP);
        consume(compiler, TOKEN_RIGHT_PAREN, "Expect ')' after for clauses.");

        emitLoop(compiler, compiler->loop->start);
        compiler->loop->start = incrementStart;

        patchJump(compiler, bodyJump);
    }

    // Compile the body.
    compiler->loop->body = compiler->function->chunk.count;
    statement(compiler);

    // Jump back to the beginning (or the increment).
    emitLoop(compiler, compiler->loop->start);

    endLoop(compiler);
    endScope(compiler); // Loop variable.
}

static void breakStatement(Compiler *compiler) {
    if (compiler->loop == NULL) {
        error(compiler->parser, "Cannot utilise 'break' outside of a loop.");
        return;
    }

    consume(compiler, TOKEN_SEMICOLON, "Expected semicolon after break");

    // Discard any locals created inside the loop.
    for (int i = compiler->localCount - 1;
         i >= 0 && compiler->locals[i].depth > compiler->loop->scopeDepth;
         i--) {
        emitByte(compiler, OP_POP);
    }

    emitJump(compiler, OP_BREAK);
}

static void continueStatement(Compiler *compiler) {
    if (compiler->loop == NULL) {
        error(compiler->parser, "Cannot utilise 'continue' outside of a loop.");
    }

    consume(compiler, TOKEN_SEMICOLON, "Expect ';' after 'continue'.");

    // Discard any locals created inside the loop.
    for (int i = compiler->localCount - 1;
         i >= 0 && compiler->locals[i].depth > compiler->loop->scopeDepth;
         i--) {
        emitByte(compiler, OP_POP);
    }

    // Jump to top of current innermost loop.
    emitLoop(compiler, compiler->loop->start);
}

static void ifStatement(Compiler *compiler) {
    consume(compiler, TOKEN_LEFT_PAREN, "Expect '(' after 'if'.");
    expression(compiler);
    consume(compiler, TOKEN_RIGHT_PAREN, "Expect ')' after condition.");

    // Jump to the else branch if the condition is false.
    int elseJump = emitJump(compiler, OP_JUMP_IF_FALSE);

    // Compile the then branch.
    emitByte(compiler, OP_POP); // Condition.
    statement(compiler);

    // Jump over the else branch when the if branch is taken.
    int endJump = emitJump(compiler, OP_JUMP);

    // Compile the else branch.
    patchJump(compiler, elseJump);
    emitByte(compiler, OP_POP); // Condition.

    if (match(compiler, TOKEN_ELSE)) statement(compiler);

    patchJump(compiler, endJump);
}

static void withStatement(Compiler *compiler) {
    consume(compiler, TOKEN_LEFT_PAREN, "Expect '(' after 'with'.");
    expression(compiler);
    consume(compiler, TOKEN_COMMA, "Expect comma");
    expression(compiler);
    consume(compiler, TOKEN_RIGHT_PAREN, "Expect ')' after 'with'.");

    beginScope(compiler);

    Local *local = &compiler->locals[compiler->localCount++];
    local->depth = compiler->scopeDepth;
    local->isUpvalue = false;
    local->name = syntheticToken("file");

    emitByte(compiler, OP_OPEN_FILE);
    statement(compiler);
    emitByte(compiler, OP_CLOSE_FILE);
    endScope(compiler);
}

static void returnStatement(Compiler *compiler) {
    if (compiler->type == TYPE_TOP_LEVEL) {
        error(compiler->parser, "Cannot return from top-level code.");
    }

    if (match(compiler, TOKEN_SEMICOLON)) {
        emitReturn(compiler);
    } else {
        if (compiler->type == TYPE_INITIALIZER) {
            error(compiler->parser, "Cannot return a value from an initializer.");
        }

        expression(compiler);
        consume(compiler, TOKEN_SEMICOLON, "Expect ';' after return value.");
        emitByte(compiler, OP_RETURN);
    }
}

static void importStatement(Compiler *compiler) {
    consume(compiler, TOKEN_STRING, "Expect string after import.");
    emitConstant(compiler, OBJ_VAL(copyString(
            compiler->parser->vm,
            compiler->parser->previous.start + 1,
            compiler->parser->previous.length - 2))
    );
    consume(compiler, TOKEN_SEMICOLON, "Expect ';' after import.");

    emitByte(compiler, OP_IMPORT);
    emitByte(compiler, OP_POP);
}

static void whileStatement(Compiler *compiler) {
    Loop loop;
    loop.start = currentChunk(compiler)->count;
    loop.scopeDepth = compiler->scopeDepth;
    loop.enclosing = compiler->loop;
    compiler->loop = &loop;

    if (check(compiler, TOKEN_LEFT_BRACE)) {
        emitByte(compiler, OP_TRUE);
    } else {
        consume(compiler, TOKEN_LEFT_PAREN, "Expect '(' after 'while'.");
        expression(compiler);
        consume(compiler, TOKEN_RIGHT_PAREN, "Expect ')' after condition.");
    }

    // Jump out of the loop if the condition is false.
    compiler->loop->end = emitJump(compiler, OP_JUMP_IF_FALSE);

    // Compile the body.
    emitByte(compiler, OP_POP); // Condition.
    compiler->loop->body = compiler->function->chunk.count;
    statement(compiler);

    // Loop back to the start.
    emitLoop(compiler, loop.start);
    endLoop(compiler);
}

static void synchronize(Parser *parser) {
    parser->panicMode = false;

    while (parser->current.type != TOKEN_EOF) {
        if (parser->previous.type == TOKEN_SEMICOLON) return;

        switch (parser->current.type) {
            case TOKEN_CLASS:
            case TOKEN_TRAIT:
            case TOKEN_DEF:
            case TOKEN_STATIC:
            case TOKEN_VAR:
            case TOKEN_FOR:
            case TOKEN_IF:
            case TOKEN_WHILE:
            case TOKEN_BREAK:
            case TOKEN_RETURN:
            case TOKEN_IMPORT:
            case TOKEN_WITH:
                return;

            default:
                // Do nothing.
                ;
        }

        advance(parser);
    }
}

static void declaration(Compiler *compiler) {
    if (match(compiler, TOKEN_CLASS)) {
        classDeclaration(compiler);
    } else if (match(compiler, TOKEN_TRAIT)) {
        traitDeclaration(compiler);
    } else if (match(compiler, TOKEN_DEF)) {
        funDeclaration(compiler);
    } else if (match(compiler, TOKEN_VAR)) {
        varDeclaration(compiler);
    } else {
        statement(compiler);
    }

    if (compiler->parser->panicMode) synchronize(compiler->parser);
}

static void statement(Compiler *compiler) {
    if (match(compiler, TOKEN_FOR)) {
        forStatement(compiler);
    } else if (match(compiler, TOKEN_IF)) {
        ifStatement(compiler);
    } else if (match(compiler, TOKEN_RETURN)) {
        returnStatement(compiler);
    } else if (match(compiler, TOKEN_WITH)) {
        withStatement(compiler);
    } else if (match(compiler, TOKEN_IMPORT)) {
        importStatement(compiler);
    } else if (match(compiler, TOKEN_BREAK)) {
        breakStatement(compiler);
    } else if (match(compiler, TOKEN_WHILE)) {
        whileStatement(compiler);
    } else if (match(compiler, TOKEN_LEFT_BRACE)) {
        Parser *parser = compiler->parser;
        Token previous = parser->previous;
        Token curtok = parser->current;

        // Advance the parser to the next token
        advance(parser);

        if (check(compiler, TOKEN_RIGHT_BRACE)) {
            if (check(compiler, TOKEN_SEMICOLON)) {
                backTrack();
                backTrack();
                parser->current = previous;
                expressionStatement(compiler);
                return;
            }
        }

        if (check(compiler, TOKEN_COLON)) {
            for (int i = 0; i < parser->current.length + parser->previous.length; ++i) {
                backTrack();
            }

            parser->current = previous;
            expressionStatement(compiler);
            return;
        }

        // Reset the scanner to the previous position
        for (int i = 0; i < parser->current.length; ++i) {
            backTrack();
        }

        // Reset the parser
        parser->previous = previous;
        parser->current = curtok;

        beginScope(compiler);
        block(compiler);
        endScope(compiler);
    } else if (match(compiler, TOKEN_CONTINUE)) {
        continueStatement(compiler);
    } else {
        expressionStatement(compiler);
    }
}

ObjFunction *compile(VM *vm, const char *source) {
    Parser parser;
    parser.vm = vm;
    parser.hadError = false;
    parser.panicMode = false;

    initScanner(source);
    Compiler compiler;
    initCompiler(&parser, &compiler, NULL, TYPE_TOP_LEVEL);

    advance(compiler.parser);

    if (!match(&compiler, TOKEN_EOF)) {
        do {
            declaration(&compiler);
        } while (!match(&compiler, TOKEN_EOF));
    }

    ObjFunction *function = endCompiler(&compiler);

    // If there was a compile error, the code is not valid, so don't
    // create a function.
    return parser.hadError ? NULL : function;
}

void grayCompilerRoots(VM *vm) {
    Compiler *compiler = vm->compiler;

    while (compiler != NULL) {
        grayObject(vm, (Obj *) compiler->function);
        grayTable(vm, &compiler->stringConstants);
        compiler = compiler->enclosing;
    }
}
