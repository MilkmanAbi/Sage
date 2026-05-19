// include/token.h
#ifndef SAGE_TOKEN_H
#define SAGE_TOKEN_H

typedef enum {
    // Keywords
    TOKEN_LET, TOKEN_VAR, TOKEN_PROC, TOKEN_IF, TOKEN_ELSE,
    TOKEN_WHILE, TOKEN_FOR, TOKEN_RETURN, TOKEN_PRINT,
    TOKEN_AND, TOKEN_OR, TOKEN_NOT, TOKEN_IN, TOKEN_BREAK, TOKEN_CONTINUE,
    TOKEN_CLASS, TOKEN_SELF, TOKEN_INIT, TOKEN_SUPER,
    
    // Phase 7: Advanced Control Flow
    TOKEN_MATCH, TOKEN_CASE, TOKEN_DEFAULT,
    TOKEN_TRY, TOKEN_CATCH, TOKEN_FINALLY, TOKEN_RAISE,
    TOKEN_DEFER, TOKEN_YIELD,
    TOKEN_ASYNC, TOKEN_AWAIT, TOKEN_SPAWN,
    
    // Phase 1.7: Data Modeling
    TOKEN_STRUCT, TOKEN_ENUM, TOKEN_TRAIT, TOKEN_CONST, TOKEN_IMPL,
    // P2: Type-name keywords for C++-style declarations: int x = 12
    TOKEN_TYPE_INT, TOKEN_TYPE_FLOAT, TOKEN_TYPE_DOUBLE, TOKEN_TYPE_STR,
    TOKEN_TYPE_BOOL, TOKEN_TYPE_NUM, TOKEN_TYPE_ANY, TOKEN_TYPE_BYTE,

    // Phase 1.8: Systems Layer
    TOKEN_UNSAFE,
    TOKEN_END,

    // Phase 8: Module System
    TOKEN_IMPORT, TOKEN_FROM, TOKEN_AS,

    // Phase 17: Metaprogramming
    TOKEN_COMPTIME,     // comptime keyword
    TOKEN_MACRO,        // macro keyword
    TOKEN_QUOTE,        // quote keyword
    TOKEN_UNQUOTE,      // unquote keyword
    TOKEN_AT,           // @ symbol for pragmas/decorators

    // Symbols & Operators
    TOKEN_LPAREN, TOKEN_RPAREN, TOKEN_PLUS, TOKEN_MINUS,
    TOKEN_STAR, TOKEN_SLASH, TOKEN_PERCENT, TOKEN_ASSIGN, TOKEN_EQ, TOKEN_NEQ,
    TOKEN_LT, TOKEN_GT, TOKEN_LTE, TOKEN_GTE, TOKEN_COLON,
    TOKEN_COMMA, TOKEN_LBRACKET, TOKEN_RBRACKET,
    TOKEN_LBRACE, TOKEN_RBRACE, TOKEN_DOT, TOKEN_ARROW,

    // Phase 9: Bitwise Operators
    TOKEN_AMP, TOKEN_PIPE, TOKEN_CARET, TOKEN_TILDE,
    TOKEN_LSHIFT, TOKEN_RSHIFT,
    // Phase 2: Option/Result operators and ranges
    TOKEN_QUESTION,      // ?    — propagation / optional chaining  
    TOKEN_BANG,          // !    — force-unwrap
    TOKEN_NULLCOAL,      // ??   — null coalesce
    TOKEN_DOTDOT,        // ..   — exclusive range
    TOKEN_DOTDOTEQ,      // ..=  — inclusive range
    TOKEN_DOTQUESTION,   // ?.   — optional member access
    TOKEN_PLUSASSIGN,    // +=
    TOKEN_MINUSASSIGN,   // -=
    TOKEN_STARASSIGN,    // *=
    TOKEN_SLASHASSIGN,   // /=
    TOKEN_FAT_ARROW,     // =>   — match arm

    // Literals & Identifiers
    TOKEN_IDENTIFIER, TOKEN_NUMBER, TOKEN_STRING, TOKEN_TRUE,
    TOKEN_FALSE, TOKEN_NIL,

    // Structural
    TOKEN_INDENT, TOKEN_DEDENT, TOKEN_NEWLINE,
    TOKEN_DOC_COMMENT, // Phase 1.9: ## doc comments
    TOKEN_EOF, TOKEN_ERROR
} TokenType;

typedef struct {
    TokenType type;
    const char* start;
    int length;
    int line;
    int column;
    const char* line_start;
    const char* filename;
} Token;

#endif
