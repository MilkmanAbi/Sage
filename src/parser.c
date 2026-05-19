#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "diagnostic.h"
#include "lexer.h"
#include "parser.h"
#include "ast.h"
#include "token.h"
#include "gc.h"
#include "repl.h"

static Token current_token;
static Token previous_token;

// Parser recursion depth limit to prevent stack overflow on malicious input
#define MAX_PARSER_DEPTH 500
static int parser_depth = 0;

static int token_span(const Token* token) {
    return (token != NULL && token->length > 0) ? token->length : 1;
}

static void parser_report(Token token, int span, const char* message, const char* help) {
    sage_print_token_diagnosticf("error", &token, NULL, span > 0 ? span : 1,
                                 help, "%s", message);
    sage_error_exit();
}

static const char* parser_expected_help(TokenType expected, Token got) {
    switch (expected) {
        case TOKEN_COLON:
            if (got.type == TOKEN_NEWLINE || got.type == TOKEN_EOF) {
                return "add ':' before the end of the line to start the block";
            }
            return "blocks in Sage start with ':'";
        case TOKEN_NEWLINE:
            if (got.type == TOKEN_EOF) {
                return "add a newline to finish this statement";
            }
            return "put the block on the next line after ':'";
        case TOKEN_IDENTIFIER:
            return "identifiers start with a letter or '_'";
        case TOKEN_RPAREN:
            return "check for a missing ')' earlier in this expression";
        case TOKEN_RBRACKET:
            return "check for a missing ']'";
        case TOKEN_RBRACE:
            return "check for a missing '}'";
        default:
            break;
    }

    if (got.type == TOKEN_NEWLINE) {
        return "this line ended before the statement was complete";
    }
    if (got.type == TOKEN_EOF) {
        return "the file ended before the statement was complete";
    }
    return NULL;
}

static void parser_expected_error(Token token, TokenType expected, const char* message) {
    char formatted[512];
    const char* found = sage_token_display_name(token.type);
    size_t message_len = strlen(message);
    if (message_len > 0 && message[message_len - 1] == '.') {
        message_len--;
    }

    if (strncmp(message, "Expect ", 7) == 0 && message_len > 7) {
        snprintf(formatted, sizeof(formatted), "expected %.*s, found %s",
                 (int)(message_len - 7), message + 7, found);
    } else {
        snprintf(formatted, sizeof(formatted), "%.*s (found %s)",
                 (int)message_len, message, found);
    }

    parser_report(token, token_span(&token), formatted,
                  parser_expected_help(expected, token));
}

static const char* parser_expression_help(Token token) {
    switch (token.type) {
        case TOKEN_NEWLINE:
            return "the line ended where Sage expected a value or expression";
        case TOKEN_COLON:
            return "did you forget the condition or value before ':'?";
        case TOKEN_RPAREN:
            return "there may be an extra ')'";
        case TOKEN_RBRACKET:
            return "there may be an extra ']'";
        case TOKEN_RBRACE:
            return "there may be an extra '}'";
        case TOKEN_EOF:
            return "the file ended before the expression was complete";
        default:
            return "expressions can start with names, literals, '(', '[', or '{'";
    }
}

static void advance_parser(void) {
    previous_token = current_token;
    current_token = scan_token();
    if (current_token.type == TOKEN_ERROR) {
        parser_report(current_token, 1, current_token.start, NULL);
    }
}

void parser_init(void) {
    advance_parser();
}

ParserState parser_get_state(void) {
    ParserState state;
    state.current_token = current_token;
    state.previous_token = previous_token;
    return state;
}

void parser_set_state(ParserState state) {
    current_token = state.current_token;
    previous_token = state.previous_token;
}

static int check(TokenType type) {
    return current_token.type == type;
}

static int match(TokenType type) {
    if (current_token.type == type) {
        advance_parser();
        return 1;
    }
    return 0;
}

static void consume(TokenType type, const char* message) {
    if (current_token.type == type) {
        advance_parser();
        return;
    }
    parser_expected_error(current_token, type, message);
}

static char* process_string_escapes(const char* src, int src_len, int* out_len) {
    char* buf = SAGE_ALLOC(src_len + 1);
    int j = 0;
    for (int i = 0; i < src_len; i++) {
        if (src[i] == '\\' && i + 1 < src_len) {
            i++;
            switch (src[i]) {
                case 'n':  buf[j++] = '\n'; break;
                case 't':  buf[j++] = '\t'; break;
                case 'r':  buf[j++] = '\r'; break;
                case '\\': buf[j++] = '\\'; break;
                case '"':  buf[j++] = '"';  break;
                case '\'': buf[j++] = '\''; break;
                case '0':  buf[j++] = '\0'; break;
                case 'a':  buf[j++] = '\a'; break;
                case 'b':  buf[j++] = '\b'; break;
                case 'f':  buf[j++] = '\f'; break;
                case 'v':  buf[j++] = '\v'; break;
                case 'x': {
                    if (i + 2 < src_len) {
                        char hex[3] = { src[i+1], src[i+2], '\0' };
                        char* end;
                        long val = strtol(hex, &end, 16);
                        if (end == hex + 2) {
                            buf[j++] = (char)val;
                            i += 2;
                        } else {
                            buf[j++] = '\\';
                            buf[j++] = 'x';
                        }
                    } else {
                        buf[j++] = '\\';
                        buf[j++] = 'x';
                    }
                    break;
                }
                default:
                    buf[j++] = '\\';
                    buf[j++] = src[i];
                    break;
            }
        } else {
            buf[j++] = src[i];
        }
    }
    buf[j] = '\0';
    if (out_len) *out_len = j;
    return buf;
}

// P1: Determine if a number token is an integer (no decimal point, no exponent)
static int token_is_integer_literal(Token token) {
    // 0b, 0x, 0o are always integer
    if (token.length >= 2 && token.start[0] == '0') {
        char c = token.start[1];
        if (c == 'b' || c == 'B' || c == 'x' || c == 'X' || c == 'o' || c == 'O')
            return 1;
    }
    // Check for '.' or 'e'/'E' — those make it a float
    for (int i = 0; i < token.length; i++) {
        char c = token.start[i];
        if (c == '.' || c == 'e' || c == 'E') return 0;
    }
    return 1;
}

static int64_t parse_int_literal(Token token) {
    if (token.length >= 2 && token.start[0] == '0' &&
        (token.start[1] == 'b' || token.start[1] == 'B')) {
        int64_t value = 0;
        for (int i = 2; i < token.length; i++) {
            if (token.start[i] == '_') continue;
            value = (value * 2) + (token.start[i] - '0');
        }
        return value;
    }

    if (token.length >= 2 && token.start[0] == '0' &&
        (token.start[1] == 'x' || token.start[1] == 'X')) {
        int64_t value = 0;
        for (int i = 2; i < token.length; i++) {
            if (token.start[i] == '_') continue;
            char c = token.start[i];
            int digit;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (c >= 'a' && c <= 'f') digit = 10 + (c - 'a');
            else digit = 10 + (c - 'A');
            value = (value * 16) + digit;
        }
        return value;
    }

    if (token.length >= 2 && token.start[0] == '0' &&
        (token.start[1] == 'o' || token.start[1] == 'O')) {
        int64_t value = 0;
        for (int i = 2; i < token.length; i++) {
            if (token.start[i] == '_') continue;
            value = (value * 8) + (token.start[i] - '0');
        }
        return value;
    }

    // Decimal integer — skip underscores
    int64_t value = 0;
    for (int i = 0; i < token.length; i++) {
        if (token.start[i] == '_') continue;
        value = value * 10 + (token.start[i] - '0');
    }
    return value;
}

static double parse_number_literal(Token token) {
    // For floats, delegate to strtod (handles 'e' notation etc.)
    char inline_buffer[128];
    char* heap_buffer = NULL;
    char* parse_buffer = inline_buffer;

    if (token.length >= (int)sizeof(inline_buffer)) {
        heap_buffer = SAGE_ALLOC((size_t)token.length + 1);
        parse_buffer = heap_buffer;
    }

    // Strip underscores for strtod
    int j = 0;
    for (int i = 0; i < token.length; i++) {
        if (token.start[i] != '_')
            parse_buffer[j++] = token.start[i];
    }
    parse_buffer[j] = '\0';

    double value = strtod(parse_buffer, NULL);
    free(heap_buffer);
    return value;
}

// Forward declarations
static Expr* expression(void);
static Expr* unary(void);
static Expr* postfix(void);
static Stmt* extern_proc_declaration(Pragma* pragma_list);
static Stmt* declaration(void);
static Stmt* statement(void);
static Stmt* block(void);
static char* take_pending_doc(void);
static TypeAnnotation* parse_type_annotation(void);

static Stmt* for_statement() {
    if (!check(TOKEN_IDENTIFIER)) {
        parser_report(current_token, token_span(&current_token),
                      "expected loop variable after 'for'",
                      "for loops need a variable name: for item in values:");
    }
    
    Token var = current_token;
    advance_parser();

    consume(TOKEN_IN, "Expect 'in' after loop variable.");

    Expr* iterable = expression();
    consume(TOKEN_COLON, "Expect ':' after for clause.");
    consume(TOKEN_NEWLINE, "Expect newline after for clause.");

    Stmt* body = block();

    return new_for_stmt(var, iterable, body);
}

// PHASE 7: Try/catch/finally statement
static Stmt* try_statement() {
    // try:
    consume(TOKEN_COLON, "Expect ':' after 'try'.");
    consume(TOKEN_NEWLINE, "Expect newline after try.");
    Stmt* try_block = block();
    
    // Parse catch clauses
    CatchClause** catches = NULL;
    int catch_count = 0;
    int catch_capacity = 0;
    
    while (match(TOKEN_CATCH)) {
        // catch e:
        consume(TOKEN_IDENTIFIER, "Expect exception variable after 'catch'.");
        Token exception_var = previous_token;
        
        consume(TOKEN_COLON, "Expect ':' after catch variable.");
        consume(TOKEN_NEWLINE, "Expect newline after catch clause.");
        Stmt* catch_body = block();
        
        CatchClause* clause = new_catch_clause(exception_var, catch_body);
        
        if (catch_count >= catch_capacity) {
            catch_capacity = catch_capacity == 0 ? 2 : catch_capacity * 2;
            catches = SAGE_REALLOC(catches, sizeof(CatchClause*) * catch_capacity);
        }
        catches[catch_count++] = clause;
    }
    
    // Optional finally:
    Stmt* finally_block = NULL;
    if (match(TOKEN_FINALLY)) {
        consume(TOKEN_COLON, "Expect ':' after 'finally'.");
        consume(TOKEN_NEWLINE, "Expect newline after finally.");
        finally_block = block();
    }
    
    return new_try_stmt(try_block, catches, catch_count, finally_block);
}

// PHASE 7: Raise statement  
static Stmt* raise_statement() {
    // raise expression
    Expr* exception = expression();
    return new_raise_stmt(exception);
}

// Defer statement: defer: <block>  or  defer <statement>
static Stmt* defer_statement() {
    if (match(TOKEN_COLON)) {
        // defer:
        //     <block>
        consume(TOKEN_NEWLINE, "Expect newline after 'defer:'.");
        Stmt* body = block();
        return new_defer_stmt(body);
    }
    // defer <single-statement>
    Stmt* body = statement();
    return new_defer_stmt(body);
}

// Match statement: match <expr>:
//     case <pattern>:
//         <block>
//     default:
//         <block>
static Stmt* match_statement() {
    Expr* value = expression();
    consume(TOKEN_COLON, "Expect ':' after match expression.");
    consume(TOKEN_NEWLINE, "Expect newline after 'match:'.");
    consume(TOKEN_INDENT, "Expect indented block after 'match:'.");

    CaseClause** cases = NULL;
    int case_count = 0;
    int case_capacity = 0;
    Stmt* default_case = NULL;

    while (!check(TOKEN_DEDENT) && !check(TOKEN_EOF)) {
        while (match(TOKEN_NEWLINE));
        if (check(TOKEN_DEDENT) || check(TOKEN_EOF)) break;

        // ── old syntax: default: ──────────────────────────────────────────
        if (match(TOKEN_DEFAULT)) {
            consume(TOKEN_COLON, "Expect ':' after 'default'.");
            while (match(TOKEN_NEWLINE));
            default_case = block();
            continue;
        }

        // ── old syntax: case pattern [if guard]: ─────────────────────────
        if (match(TOKEN_CASE)) {
            Expr* pattern = expression();
            Expr* guard = NULL;
            if (match(TOKEN_IF)) guard = expression();
            consume(TOKEN_COLON, "Expect ':' after case pattern.");
            while (match(TOKEN_NEWLINE));
            Stmt* body = block();
            CaseClause* clause = new_case_clause(pattern, body);
            clause->guard = guard;
            if (case_count >= case_capacity) {
                case_capacity = case_capacity == 0 ? 4 : case_capacity * 2;
                cases = SAGE_REALLOC(cases, sizeof(CaseClause*) * case_capacity);
            }
            cases[case_count++] = clause;
            continue;
        }

        // ── new syntax:  pattern [if guard] => stmt_or_block ─────────────
        // Pattern can be:
        //   _                     wildcard
        //   literal               42, "hi", true, nil
        //   lo..hi / lo..=hi      range
        //   Name                  binding capture (identifier, not call)
        //   Name(binding)         ADT destructure: Some(x), Ok(v), Err(e)
        //   n if expr             guard
        //   _ | _ | _             or-patterns (pipe-separated)
        {
            // Parse one arm: pattern [if guard] =>
            Expr* pattern = expression();   // covers all pattern forms
            Expr* guard = NULL;
            if (match(TOKEN_IF)) guard = expression();

            if (match(TOKEN_FAT_ARROW)) {
                Stmt* body = NULL;
                while (match(TOKEN_NEWLINE));
                if (check(TOKEN_INDENT)) {
                    body = block();
                } else {
                    // Single statement arm: pattern => stmt (return, let, expr, etc.)
                    body = declaration();
                    match(TOKEN_NEWLINE);
                }

                // Check if this is a wildcard/default pattern
                int is_wildcard = (pattern->type == EXPR_VARIABLE
                    && pattern->as.variable.name.length == 1
                    && pattern->as.variable.name.start[0] == '_');

                if (is_wildcard && guard == NULL) {
                    default_case = body;
                } else {
                    CaseClause* clause = new_case_clause(pattern, body);
                    clause->guard = guard;
                    if (case_count >= case_capacity) {
                        case_capacity = case_capacity == 0 ? 4 : case_capacity * 2;
                        cases = SAGE_REALLOC(cases, sizeof(CaseClause*) * case_capacity);
                    }
                    cases[case_count++] = clause;
                }
                continue;
            }

            // Neither case/default nor fat-arrow — parse error
            parser_report(current_token, token_span(&current_token),
                          "Expect '=>', 'case', or 'default' inside match block.", NULL);
            sage_error_exit();
            break;
        }
    }

    if (check(TOKEN_DEDENT)) advance_parser();
    return new_match_stmt(value, cases, case_count, default_case);
}

// PHASE 7: Yield statement
static Stmt* yield_statement() {
    // yield <expression>
    // yield can also be used without a value: yield (yields nil)
    Expr* value = NULL;
    
    if (!check(TOKEN_NEWLINE) && !check(TOKEN_EOF) && !check(TOKEN_DEDENT)) {
        value = expression();
    }
    
    return new_yield_stmt(value);
}

// PHASE 8: Import statement - WITH ALIASING SUPPORT
// Accept any identifier-like token (including keywords) for module paths
static int match_identifier_like(void) {
    if (current_token.type == TOKEN_IDENTIFIER ||
        current_token.type == TOKEN_ENUM ||
        current_token.type == TOKEN_STRUCT ||
        current_token.type == TOKEN_TRAIT ||
        current_token.type == TOKEN_MATCH ||
        current_token.type == TOKEN_INIT) {
        advance_parser();
        return 1;
    }
    return 0;
}

static Stmt* import_statement() {
    // Three forms:
    // 1. import module_name
    // 2. import module_name as alias
    // 3. from module_name import item1, item2, ...
    // 4. from module_name import item1 as alias1, item2 as alias2
    
    if (match(TOKEN_FROM)) {
        // from module_name import item1 [as alias1], item2 [as alias2], ...
        consume(TOKEN_IDENTIFIER, "Expect module name after 'from'.");
        Token module_token = previous_token;

        // Build dotted module name (e.g., graphics.vulkan)
        int name_len = module_token.length;
        char* module_name = SAGE_ALLOC(name_len + 1);
        memcpy(module_name, module_token.start, module_token.length);
        module_name[name_len] = '\0';

        while (match(TOKEN_DOT)) {
            if (!match_identifier_like()) {
                parser_report(current_token, token_span(&current_token),
                              "expected identifier after '.' in module name", NULL);
                break;
            }
            Token part = previous_token;
            int new_len = name_len + 1 + part.length;
            module_name = SAGE_REALLOC(module_name, new_len + 1);
            module_name[name_len] = '.';
            memcpy(module_name + name_len + 1, part.start, part.length);
            name_len = new_len;
            module_name[name_len] = '\0';
        }

        consume(TOKEN_IMPORT, "Expect 'import' after module name.");

        // Check for wildcard import: from module import *
        if (match(TOKEN_STAR)) {
            // Use import_all=0 with a special "*" item to distinguish
            // from "import module" (which uses import_all=1)
            char** star_items = SAGE_ALLOC(sizeof(char*));
            char** star_aliases = SAGE_ALLOC(sizeof(char*));
            star_items[0] = SAGE_STRDUP("*");
            star_aliases[0] = NULL;
            return new_import_stmt(module_name, star_items, star_aliases, 1, NULL, 0);
        }

        // Parse imported items and their aliases
        char** items = NULL;
        char** item_aliases = NULL;
        int item_count = 0;
        int capacity = 0;

        do {
            consume(TOKEN_IDENTIFIER, "Expect identifier in import list.");
            Token item_token = previous_token;
            
            if (item_count >= capacity) {
                capacity = capacity == 0 ? 4 : capacity * 2;
                items = SAGE_REALLOC(items, sizeof(char*) * capacity);
                item_aliases = SAGE_REALLOC(item_aliases, sizeof(char*) * capacity);  // ✅ NEW
            }
            
            // Store the original item name
            items[item_count] = SAGE_ALLOC(item_token.length + 1);
            memcpy(items[item_count], item_token.start, item_token.length);
            items[item_count][item_token.length] = '\0';
            
            // ✅ NEW: Check for 'as alias'
            if (match(TOKEN_AS)) {
                consume(TOKEN_IDENTIFIER, "Expect alias name after 'as'.");
                Token alias_token = previous_token;
                
                item_aliases[item_count] = SAGE_ALLOC(alias_token.length + 1);
                memcpy(item_aliases[item_count], alias_token.start, alias_token.length);
                item_aliases[item_count][alias_token.length] = '\0';
            } else {
                item_aliases[item_count] = NULL;  // No alias for this item
            }
            
            item_count++;
            
        } while (match(TOKEN_COMMA));
        
        return new_import_stmt(module_name, items, item_aliases, item_count, NULL, 0);
    }
    
    // import module_name [as alias]
    consume(TOKEN_IDENTIFIER, "Expect module name after 'import'.");
    Token module_token = previous_token;

    // Build dotted module name (e.g., graphics.vulkan)
    int name_len = module_token.length;
    char* module_name = SAGE_ALLOC(name_len + 1);
    memcpy(module_name, module_token.start, module_token.length);
    module_name[name_len] = '\0';

    while (match(TOKEN_DOT)) {
        if (!match_identifier_like()) {
            parser_report(current_token, token_span(&current_token),
                          "expected identifier after '.' in module name", NULL);
            break;
        }
        Token part = previous_token;
        int new_len = name_len + 1 + part.length;
        module_name = SAGE_REALLOC(module_name, new_len + 1);
        module_name[name_len] = '.';
        memcpy(module_name + name_len + 1, part.start, part.length);
        name_len = new_len;
        module_name[name_len] = '\0';
    }
    
    char* alias = NULL;
    if (match(TOKEN_AS)) {
        consume(TOKEN_IDENTIFIER, "Expect alias after 'as'.");
        Token alias_token = previous_token;
        
        alias = SAGE_ALLOC(alias_token.length + 1);
        memcpy(alias, alias_token.start, alias_token.length);
        alias[alias_token.length] = '\0';
    }
    
    // import_all = 1 (importing entire module)
    // item_aliases = NULL (not used for module-level imports)
    return new_import_stmt(module_name, NULL, NULL, 0, alias, 1);
}

// primary -> NUMBER | STRING | BOOLEAN | NIL | SELF | ( expr/tuple ) | [ array ] | { dict } | IDENTIFIER | CALL
static Expr* primary() {
    // Literals
    if (match(TOKEN_FALSE)) return new_bool_expr(0);
    if (match(TOKEN_TRUE))  return new_bool_expr(1);
    if (match(TOKEN_NIL))   return new_nil_expr();
    
    // Self keyword (treated as variable)
    if (match(TOKEN_SELF)) {
        Token self_token = previous_token;
        return new_variable_expr(self_token);
    }

    // Super keyword: super.method(args) calls parent class method
    if (match(TOKEN_SUPER)) {
        if (match(TOKEN_DOT) || match(TOKEN_ARROW)) {
            // Accept both identifiers and 'init' keyword as method names
            if (!match(TOKEN_IDENTIFIER) && !match(TOKEN_INIT)) {
                parser_report(current_token, token_span(&current_token),
                              "expect method name after 'super.'",
                              "use 'super.init(self, args)' or 'super.method(self, args)'");
            }
            Token method = previous_token;
            return new_super_expr(method);
        }
        parser_report(current_token, token_span(&current_token),
                      "expect '.' or '->' after 'super'",
                      "use 'super.init(self, args)' to call parent method");
    }

    // Phase 17: comptime expression — comptime(expr) evaluates at compile time
    if (match(TOKEN_COMPTIME)) {
        consume(TOKEN_LPAREN, "Expect '(' after 'comptime' in expression context.");
        Expr* inner = expression();
        consume(TOKEN_RPAREN, "Expect ')' after comptime expression.");
        return new_comptime_expr(inner);
    }

    // Parentheses: ( expr ) or tuple (a, b, c)
    if (match(TOKEN_LPAREN)) {
        if (match(TOKEN_RPAREN)) {
            return new_tuple_expr(NULL, 0);
        }

        Expr* first = expression();
        
        if (match(TOKEN_COMMA)) {
            Expr** elements = NULL;
            int count = 0;
            int capacity = 4;
            elements = SAGE_ALLOC(sizeof(Expr*) * capacity);
            elements[count++] = first;
            
            if (!check(TOKEN_RPAREN)) {
                do {
                    if (count >= capacity) {
                        capacity *= 2;
                        elements = SAGE_REALLOC(elements, sizeof(Expr*) * capacity);
                    }
                    elements[count++] = expression();
                } while (match(TOKEN_COMMA) && !check(TOKEN_RPAREN));
            }
            
            consume(TOKEN_RPAREN, "Expect ')' after tuple elements.");
            return new_tuple_expr(elements, count);
        }
        
        consume(TOKEN_RPAREN, "Expect ')' after expression.");
        return first;
    }

    // Dictionary Literals
    if (match(TOKEN_LBRACE)) {
        char** keys = NULL;
        Expr** values = NULL;
        int count = 0;
        int capacity = 0;

        if (!check(TOKEN_RBRACE)) {
            do {
                consume(TOKEN_STRING, "Expect string key in dictionary.");
                Token key_token = previous_token;

                int len = key_token.length - 2;
                char* key = process_string_escapes(key_token.start + 1, len, NULL);
                
                consume(TOKEN_COLON, "Expect ':' after dictionary key.");
                Expr* value = expression();
                
                if (count >= capacity) {
                    capacity = capacity == 0 ? 4 : capacity * 2;
                    keys = SAGE_REALLOC(keys, sizeof(char*) * capacity);
                    values = SAGE_REALLOC(values, sizeof(Expr*) * capacity);
                }
                keys[count] = key;
                values[count] = value;
                count++;
            } while (match(TOKEN_COMMA) && !check(TOKEN_RBRACE));
        }
        
        consume(TOKEN_RBRACE, "Expect '}' after dictionary elements.");
        return new_dict_expr(keys, values, count);
    }

    // Array Literals (supports multiline, trailing commas, and spread ...arr)
    if (match(TOKEN_LBRACKET)) {
        Expr** elements = NULL;
        int count = 0;
        int capacity = 0;

        if (!check(TOKEN_RBRACKET)) {
            do {
                if (check(TOKEN_RBRACKET)) break; // trailing comma
                // Spread: ..arr or ...arr
                if (check(TOKEN_DOTDOT)) {
                    advance_parser(); // consume '..'
                    if (check(TOKEN_DOT)) advance_parser(); // optional third dot
                    Expr* target = expression();
                    // Tag as spread using binary with TOKEN_DOTDOT op and NULL right
                    Token sp = {TOKEN_DOTDOT, "..", 2, 0, 0};
                    Expr* spread = new_binary_expr(target, sp, NULL);
                    if (count >= capacity) {
                        capacity = capacity == 0 ? 4 : capacity * 2;
                        elements = SAGE_REALLOC(elements, sizeof(Expr*) * capacity);
                    }
                    elements[count++] = spread;
                } else {
                    Expr* elem = expression();
                    if (count >= capacity) {
                        capacity = capacity == 0 ? 4 : capacity * 2;
                        elements = SAGE_REALLOC(elements, sizeof(Expr*) * capacity);
                    }
                    elements[count++] = elem;
                }
            } while (match(TOKEN_COMMA));
        }
        consume(TOKEN_RBRACKET, "Expect ']' after array elements.");
        return new_array_expr(elements, count);
    }

    // Numbers
    if (match(TOKEN_NUMBER)) {
        // P1: Produce EXPR_INT for integer literals, EXPR_NUMBER for floats
        if (token_is_integer_literal(previous_token)) {
            int64_t val = parse_int_literal(previous_token);
            return new_int_expr(val);
        }
        double val = parse_number_literal(previous_token);
        return new_number_expr(val);
    }

    // Strings — with interpolation support: "Hello, {name}! {n*2}"
    if (match(TOKEN_STRING)) {
        Token t = previous_token;
        // t.start points at the opening quote, t.length includes both quotes
        const char* raw   = t.start + 1;
        int         raw_len = t.length - 2;
        if (raw_len < 0) raw_len = 0;

        // Check if string contains REAL interpolation: {expr} not bare { or {}
        int has_interp = 0;
        for (int i = 0; i < raw_len; i++) {
            if (raw[i] == '{' && (i == 0 || raw[i-1] != '\\') &&
                i + 1 < raw_len && raw[i+1] != '}') {
                has_interp = 1; break;
            }
        }

        if (!has_interp) {
            int len2 = t.length - 2;
            if (len2 < 0) len2 = 0;
            char* str = process_string_escapes(t.start + 1, len2, NULL);
            return new_string_expr(str);
        }

        // Emit EXPR_INTERP — interpolation is evaluated at runtime
        // Store the raw template (without outer quotes) and let the interpreter handle {expr}
        char* tmpl = SAGE_ALLOC(raw_len + 1);
        memcpy(tmpl, raw, raw_len);
        tmpl[raw_len] = '\0';

        Expr* interp_expr = SAGE_ALLOC(sizeof(Expr));
        interp_expr->type = EXPR_INTERP;
        interp_expr->as.interp.template_str = tmpl;
        interp_expr->as.interp.template_len = raw_len;
        return interp_expr;
    }

    // Identifiers (including keywords that can be used as variable names in expression context)
    if (match(TOKEN_IDENTIFIER) || match(TOKEN_ENUM) || match(TOKEN_STRUCT) || match(TOKEN_TRAIT) ||
        match(TOKEN_MATCH) || match(TOKEN_INIT)) {
        Token name = previous_token;
        return new_variable_expr(name);
    }

    {
        char message[256];
        snprintf(message, sizeof(message), "expected expression, found %s",
                 sage_token_display_name(current_token.type));
        parser_report(current_token, token_span(&current_token), message,
                      parser_expression_help(current_token));
    }
    return NULL;
}

// Unary expressions (handle negative numbers and bitwise NOT)
static Expr* unary() {
    // Handle unary minus: -5, -x
    if (match(TOKEN_MINUS)) {
        Token op = previous_token;
        Expr* right = unary();  // Right associative
        // Represent -x as (0 - x) — use int zero so int promotion works
        return new_binary_expr(new_int_expr(0), op, right);
    }
    if (match(TOKEN_NOT)) {
        Token op = previous_token;
        Expr* right = unary();
        return new_binary_expr(right, op, NULL);
    }
    // Phase 9: Bitwise NOT (~x)
    if (match(TOKEN_TILDE)) {
        Token op = previous_token;
        Expr* right = unary();
        return new_binary_expr(right, op, NULL);
    }
    // Phase 11: await expression
    if (match(TOKEN_AWAIT)) {
        Expr* right = unary();
        return new_await_expr(right);
    }

    return postfix();
}

static Expr* postfix() {
    Expr* expr = primary();

    while (1) {
        if (match(TOKEN_LPAREN)) {
            Expr** args = NULL;
            int count = 0;
            int capacity = 0;

            if (!check(TOKEN_RPAREN)) {
                do {
                    if (check(TOKEN_RPAREN)) break; // trailing comma
                    // Named arg: ident: expr — skip name and colon, use positional
                    if (check(TOKEN_IDENTIFIER)) {
                        LexerState _plex = lexer_get_state();
                        Token _pcur = current_token, _pprev = previous_token;
                        advance_parser();  // consume potential name
                        if (check(TOKEN_COLON)) {
                            advance_parser();  // consume ':'
                            // fall through to parse the value
                        } else {
                            // Not named: restore
                            lexer_set_state(_plex);
                            current_token  = _pcur;
                            previous_token = _pprev;
                        }
                    }
                    Expr* arg = expression();
                    if (count >= capacity) {
                        capacity = capacity == 0 ? 4 : capacity * 2;
                        args = SAGE_REALLOC(args, sizeof(Expr*) * capacity);
                    }
                    args[count++] = arg;
                } while (match(TOKEN_COMMA));
            }
            consume(TOKEN_RPAREN, "Expect ')' after arguments.");
            expr = new_call_expr(expr, args, count);
        } else if (match(TOKEN_LBRACKET)) {
            Expr* start_or_index = NULL;
            Expr* end = NULL;
            
            if (!check(TOKEN_COLON)) {
                start_or_index = expression();
            }
            
            if (match(TOKEN_COLON)) {
                Expr* start = start_or_index;
                
                if (!check(TOKEN_RBRACKET)) {
                    end = expression();
                }
                
                consume(TOKEN_RBRACKET, "Expect ']' after slice.");
                expr = new_slice_expr(expr, start, end);
            } else {
                consume(TOKEN_RBRACKET, "Expect ']' after index.");
                expr = new_index_expr(expr, start_or_index);
            }
        } else if (match(TOKEN_DOT) || match(TOKEN_ARROW)) {
            // Accept identifiers and keywords as property/method names
            if (!match(TOKEN_IDENTIFIER) && !match(TOKEN_INIT) &&
                !match(TOKEN_CLASS) && !match(TOKEN_SELF) &&
                !match(TOKEN_SUPER) && !match(TOKEN_IN) &&
                !match(TOKEN_IMPORT) && !match(TOKEN_END) &&
                !match(TOKEN_SPAWN)) {
                parser_report(current_token, token_span(&current_token),
                              "expected property name after '.' or '->'",
                              "identifiers start with a letter or '_'");
            }
            Token property = previous_token;
            expr = new_get_expr(expr, property);
        } else if (match(TOKEN_DOTQUESTION)) {
            // ?. — optional member access: expr?.member
            if (!match(TOKEN_IDENTIFIER)) {
                parser_report(current_token, token_span(&current_token),
                              "expected member name after '?.'",
                              "identifiers start with a letter or '_'");
            }
            Token member = previous_token;
            expr = new_optchain_expr(expr, member);
        } else if (match(TOKEN_BANG)) {
            // expr! — force-unwrap
            expr = new_force_unwrap_expr(expr);
        } else if (match(TOKEN_QUESTION)) {
            // expr? — propagate: early return None/Err if nil/Err
            expr = new_propagate_expr(expr);
        } else {
            break;
        }
    }

    return expr;
}

static Expr* term() {
    Expr* expr = unary();
    while (match(TOKEN_STAR) || match(TOKEN_SLASH) || match(TOKEN_PERCENT)) {
        Token op = previous_token;
        Expr* right = unary();
        expr = new_binary_expr(expr, op, right);
    }
    return expr;
}

static Expr* addition() {
    Expr* expr = term();
    while (match(TOKEN_PLUS) || match(TOKEN_MINUS)) {
        Token op = previous_token;
        Expr* right = term();
        expr = new_binary_expr(expr, op, right);
    }
    return expr;
}

// Phase 9: Shift operators (<< >>), between addition and comparison
static Expr* shift() {
    Expr* expr = addition();
    while (match(TOKEN_LSHIFT) || match(TOKEN_RSHIFT)) {
        Token op = previous_token;
        Expr* right = addition();
        expr = new_binary_expr(expr, op, right);
    }
    return expr;
}

static Expr* comparison() {
    Expr* expr = shift();
    while (match(TOKEN_GT) || match(TOKEN_LT) || match(TOKEN_GTE) || match(TOKEN_LTE)) {
        Token op = previous_token;
        Expr* right = shift();
        expr = new_binary_expr(expr, op, right);
    }
    return expr;
}

static Expr* equality() {
    Expr* expr = comparison();
    while (match(TOKEN_EQ) || match(TOKEN_NEQ)) {
        Token op = previous_token;
        Expr* right = comparison();
        expr = new_binary_expr(expr, op, right);
    }
    return expr;
}

// Phase 9: Bitwise AND (&), between equality and bitwise XOR
static Expr* bitwise_and() {
    Expr* expr = equality();
    while (match(TOKEN_AMP)) {
        Token op = previous_token;
        Expr* right = equality();
        expr = new_binary_expr(expr, op, right);
    }
    return expr;
}

// Phase 9: Bitwise XOR (^), between bitwise AND and bitwise OR
static Expr* bitwise_xor() {
    Expr* expr = bitwise_and();
    while (match(TOKEN_CARET)) {
        Token op = previous_token;
        Expr* right = bitwise_and();
        expr = new_binary_expr(expr, op, right);
    }
    return expr;
}

// Phase 9: Bitwise OR (|), between bitwise XOR and logical AND
static Expr* bitwise_or() {
    Expr* expr = bitwise_xor();
    while (match(TOKEN_PIPE)) {
        Token op = previous_token;
        Expr* right = bitwise_xor();
        expr = new_binary_expr(expr, op, right);
    }
    return expr;
}

static Expr* logical_and() {
    Expr* expr = bitwise_or();
    while (match(TOKEN_AND)) {
        Token op = previous_token;
        Expr* right = bitwise_or();
        expr = new_binary_expr(expr, op, right);
    }
    return expr;
}

static Expr* logical_or() {
    Expr* expr = logical_and();
    while (match(TOKEN_OR)) {
        Token op = previous_token;
        Expr* right = logical_and();
        expr = new_binary_expr(expr, op, right);
    }
    return expr;
}

static Expr* null_coalesce() {
    Expr* expr = logical_or();
    while (match(TOKEN_NULLCOAL)) {
        Expr* right = logical_or();
        expr = new_nullcoal_expr(expr, right);
    }
    return expr;
}

static Expr* range_expr() {
    Expr* expr = null_coalesce();
    if (match(TOKEN_DOTDOT)) {
        Expr* high = null_coalesce();
        return new_range_expr(expr, high, 0);
    }
    if (match(TOKEN_DOTDOTEQ)) {
        Expr* high = null_coalesce();
        return new_range_expr(expr, high, 1);
    }
    return expr;
}

static Expr* assignment() {
    Expr* expr = range_expr();
    
    if (expr->type == EXPR_GET && match(TOKEN_ASSIGN)) {
        Expr* object = expr->as.get.object;
        Token property = expr->as.get.property;
        free(expr);
        Expr* value = assignment();
        return new_set_expr(object, property, value);
    }
    
    // Handle index assignment: arr[i] = value, dict[key] = value
    if (expr->type == EXPR_INDEX && match(TOKEN_ASSIGN)) {
        Expr* array = expr->as.index.array;
        Expr* index = expr->as.index.index;
        free(expr);
        Expr* value = assignment();
        return new_index_set_expr(array, index, value);
    }

    // Handle regular variable assignment: x = value
    if (expr->type == EXPR_VARIABLE && match(TOKEN_ASSIGN)) {
        Token name = expr->as.variable.name;
        free(expr);
        Expr* value = assignment();
        return new_set_expr(NULL, name, value);
    }

    // Augmented assignment: x += y  =>  x = x + y  (desugared here)
    if (expr->type == EXPR_VARIABLE) {
        Token op_tok;
        TokenType bin_op = TOKEN_ERROR;
        if      (match(TOKEN_PLUSASSIGN))  { op_tok = previous_token; bin_op = TOKEN_PLUS; }
        else if (match(TOKEN_MINUSASSIGN)) { op_tok = previous_token; bin_op = TOKEN_MINUS; }
        else if (match(TOKEN_STARASSIGN))  { op_tok = previous_token; bin_op = TOKEN_STAR; }
        else if (match(TOKEN_SLASHASSIGN)) { op_tok = previous_token; bin_op = TOKEN_SLASH; }
        if (bin_op != TOKEN_ERROR) {
            Token name = expr->as.variable.name;
            Token bin_token = op_tok;
            bin_token.type = bin_op;
            Expr* var_copy = new_variable_expr(name);
            Expr* rhs = assignment();
            Expr* combined = new_binary_expr(var_copy, bin_token, rhs);
            free(expr);
            return new_set_expr(NULL, name, combined);
        }
    }

    return expr;
}

static Expr* expression() {
    if (++parser_depth > MAX_PARSER_DEPTH) {
        char message[128];
        snprintf(message, sizeof(message),
                 "maximum nesting depth exceeded (%d)", MAX_PARSER_DEPTH);
        parser_report(current_token, 1, message,
                      "reduce the depth of nested expressions");
        parser_depth--;
        return NULL;
    }
    Expr* result = assignment();
    parser_depth--;
    return result;
}

// Phase 17: comptime block — executes code at compile time
static Stmt* comptime_statement() {
    // comptime let x = expr — constant declaration (just a regular let at interpreter level)
    if (check(TOKEN_LET) || check(TOKEN_VAR)) {
        return declaration();  // let/var/const with comptime annotation
    }
    consume(TOKEN_COLON, "Expect ':' after 'comptime'.");
    consume(TOKEN_NEWLINE, "Expect newline after 'comptime:'.");
    Stmt* body = block();
    return new_comptime_stmt(body);
}

// Phase 17: macro definition — macro name(params): body
static Stmt* macro_declaration() {
    consume(TOKEN_IDENTIFIER, "Expect macro name.");
    Token name = previous_token;

    consume(TOKEN_LPAREN, "Expect '(' after macro name.");
    Token* params = NULL;
    int count = 0;
    int capacity = 0;

    if (!check(TOKEN_RPAREN)) {
        do {
            if (check(TOKEN_RPAREN)) break;
            consume(TOKEN_IDENTIFIER, "Expect parameter name.");
            if (count >= capacity) {
                capacity = capacity == 0 ? 4 : capacity * 2;
                params = SAGE_REALLOC(params, sizeof(Token) * capacity);
            }
            params[count++] = previous_token;
        } while (match(TOKEN_COMMA));
    }
    consume(TOKEN_RPAREN, "Expect ')' after macro parameters.");

    consume(TOKEN_COLON, "Expect ':' after macro signature.");
    consume(TOKEN_NEWLINE, "Expect newline before macro body.");
    Stmt* body = block();

    return new_macro_def_stmt(name, params, count, body);
}

// Phase 17: parse @pragma decorators and attach to next declaration
static Pragma* parse_pragma(void) {
    consume(TOKEN_IDENTIFIER, "Expect pragma name after '@'.");
    Token name_tok = previous_token;
    char* name = SAGE_ALLOC(name_tok.length + 1);
    memcpy(name, name_tok.start, name_tok.length);
    name[name_tok.length] = '\0';

    char** args = NULL;
    int arg_count = 0;
    int arg_capacity = 0;

    // Optional arguments: @section(".multiboot_header")
    if (match(TOKEN_LPAREN)) {
        if (!check(TOKEN_RPAREN)) {
            do {
                if (check(TOKEN_RPAREN)) break;
                if (match(TOKEN_STRING)) {
                    Token arg_tok = previous_token;
                    int len = arg_tok.length - 2;
                    char* arg = SAGE_ALLOC(len + 1);
                    memcpy(arg, arg_tok.start + 1, len);
                    arg[len] = '\0';
                    if (arg_count >= arg_capacity) {
                        arg_capacity = arg_capacity == 0 ? 4 : arg_capacity * 2;
                        args = SAGE_REALLOC(args, sizeof(char*) * arg_capacity);
                    }
                    args[arg_count++] = arg;
                } else if (match(TOKEN_IDENTIFIER) || match(TOKEN_NUMBER)) {
                    Token arg_tok = previous_token;
                    char* arg = SAGE_ALLOC(arg_tok.length + 1);
                    memcpy(arg, arg_tok.start, arg_tok.length);
                    arg[arg_tok.length] = '\0';
                    if (arg_count >= arg_capacity) {
                        arg_capacity = arg_capacity == 0 ? 4 : arg_capacity * 2;
                        args = SAGE_REALLOC(args, sizeof(char*) * arg_capacity);
                    }
                    args[arg_count++] = arg;
                } else {
                    parser_report(current_token, token_span(&current_token),
                                  "expected string or identifier argument in pragma",
                                  "pragma arguments should be strings or identifiers");
                }
            } while (match(TOKEN_COMMA));
        }
        consume(TOKEN_RPAREN, "Expect ')' after pragma arguments.");
    }

    return new_pragma(name, args, arg_count);
}

// Phase 17: parse generic type parameters [T, U, V]
static void parse_generic_type_params(Token** out_params, int* out_count) {
    *out_params = NULL;
    *out_count = 0;
    if (!match(TOKEN_LBRACKET)) return;

    int capacity = 0;
    do {
        consume(TOKEN_IDENTIFIER, "Expect type parameter name.");
        if (*out_count >= capacity) {
            capacity = capacity == 0 ? 4 : capacity * 2;
            *out_params = SAGE_REALLOC(*out_params, sizeof(Token) * capacity);
        }
        (*out_params)[(*out_count)++] = previous_token;
    } while (match(TOKEN_COMMA));
    consume(TOKEN_RBRACKET, "Expect ']' after type parameters.");
}

static Stmt* print_statement() {
    Expr* value = expression();
    return new_print_stmt(value);
}

static Stmt* block() {
    if (++parser_depth > MAX_PARSER_DEPTH) {
        char message[128];
        snprintf(message, sizeof(message),
                 "maximum nesting depth exceeded (%d)", MAX_PARSER_DEPTH);
        parser_report(current_token, 1, message,
                      "reduce the depth of nested blocks");
    }
    // Allow 'end' to immediately close an empty block (no indented body)
    while (check(TOKEN_NEWLINE)) match(TOKEN_NEWLINE);
    if (check(TOKEN_END)) {
        match(TOKEN_END);
        parser_depth--;
        return new_block_stmt(NULL);
    }
    consume(TOKEN_INDENT, "Expect indentation after block start.");

    Stmt* head = NULL;
    Stmt* current = NULL;

    while (!check(TOKEN_DEDENT) && !check(TOKEN_EOF)) {
        if (match(TOKEN_NEWLINE)) {
            continue;
        }

        Stmt* stmt = declaration();

        if (head == NULL) {
            head = stmt;
            current = head;
        } else {
            current->next = stmt;
            current = stmt;
        }
    }

    consume(TOKEN_DEDENT, "Expect dedent at end of block.");
    parser_depth--;
    return new_block_stmt(head);
}

static Stmt* if_statement() {
    Expr* condition = expression();
    consume(TOKEN_COLON, "Expect ':' after if condition.");
    consume(TOKEN_NEWLINE, "Expect newline after if condition.");
    Stmt* then_branch = block();

    Stmt* else_branch = NULL;
    if (check(TOKEN_IF) && previous_token.type == TOKEN_DEDENT) {
        // elif: check if the current TOKEN_IF was originally 'elif' in source
        // by looking at the token text (elif starts with 'e')
        if (current_token.length == 4 && current_token.start[0] == 'e') {
            advance_parser(); // consume the elif (TOKEN_IF)
            else_branch = if_statement(); // recursively parse as nested if
        }
    }
    if (else_branch == NULL && match(TOKEN_ELSE)) {
        consume(TOKEN_COLON, "Expect ':' after else.");
        consume(TOKEN_NEWLINE, "Expect newline after else.");
        else_branch = block();
    }

    return new_if_stmt(condition, then_branch, else_branch);
}

static Stmt* while_statement() {
    Expr* condition = expression();
    consume(TOKEN_COLON, "Expect ':' after while condition.");
    consume(TOKEN_NEWLINE, "Expect newline after while condition.");
    Stmt* body = block();
    return new_while_stmt(condition, body);
}

// Parse a type annotation: Int, String, Array[Int], Dict[String, Int], T?
static TypeAnnotation* parse_type_annotation(void) {
    if (current_token.type != TOKEN_IDENTIFIER) return NULL;
    Token name = current_token;
    advance_parser();

    TypeAnnotation** params = NULL;
    int param_count = 0;
    int is_optional = 0;

    // Generic parameters: Array[Int], Dict[K, V]
    if (match(TOKEN_LBRACKET)) {
        int capacity = 0;
        do {
            TypeAnnotation* param = parse_type_annotation();
            if (param) {
                if (param_count >= capacity) {
                    capacity = capacity == 0 ? 2 : capacity * 2;
                    params = SAGE_REALLOC(params, sizeof(TypeAnnotation*) * capacity);
                }
                params[param_count++] = param;
            }
        } while (match(TOKEN_COMMA));
        consume(TOKEN_RBRACKET, "Expect ']' after type parameters.");
    }

    // Optional: T?
    // Check if next char is '?' — but we don't have TOKEN_QUESTION, so skip for now

    return new_type_annotation(name, params, param_count, is_optional);
}

// Phase 4: Parse "extern proc name(arg: type, ...) -> ret_type"
// Extracts type names as strings for the libffi dispatcher.
static Stmt* extern_proc_declaration(Pragma* pragma_list) {
    if (!match(TOKEN_IDENTIFIER) && !match(TOKEN_INIT)) {
        parser_report(current_token, token_span(&current_token),
                      "expected function name after 'extern proc'", NULL);
        return NULL;
    }
    Token name = previous_token;
    consume(TOKEN_LPAREN, "Expect '(' after extern proc name.");

    Token* params = NULL;
    char** param_types = NULL;
    int count = 0, capacity = 0;

    if (!check(TOKEN_RPAREN)) {
        do {
            if (check(TOKEN_RPAREN)) break;
            // Parameter can be: name: type  or  ...varargs: any
            if (count >= capacity) {
                capacity = capacity == 0 ? 4 : capacity * 2;
                params = SAGE_REALLOC(params, sizeof(Token) * capacity);
                param_types = SAGE_REALLOC(param_types, sizeof(char*) * capacity);
            }

            // Check for variadic ...args: any
            // The lexer tokenises "..." as TOKEN_DOTDOT ("..") + TOKEN_DOT (".")
            if (check(TOKEN_DOTDOT) || check(TOKEN_DOTDOTEQ)) {
                // Skip the rest of the parameter list (variadic not supported in FFI)
                while (!check(TOKEN_RPAREN) && !check(TOKEN_NEWLINE) && !check(TOKEN_EOF))
                    advance_parser();
                break;
            }

            if (!match(TOKEN_IDENTIFIER) && !match(TOKEN_SELF)) {
                break;
            }
            params[count] = previous_token;
            param_types[count] = SAGE_STRDUP("any");

            if (match(TOKEN_COLON)) {
                // Capture the type as a string
                char type_buf[256] = {0};
                int tlen = 0;
                // Allow multi-token types: ptr<sqlite3>, Array<int>, etc.
                while (!check(TOKEN_COMMA) && !check(TOKEN_RPAREN) &&
                       !check(TOKEN_NEWLINE) && !check(TOKEN_EOF)) {
                    Token ttok = current_token;
                    if (ttok.length + tlen < (int)sizeof(type_buf)-1) {
                        memcpy(type_buf + tlen, ttok.start, ttok.length);
                        tlen += ttok.length;
                    }
                    advance_parser();
                }
                type_buf[tlen] = '\0';
                free(param_types[count]);
                param_types[count] = SAGE_STRDUP(type_buf);
            }
            count++;
        } while (match(TOKEN_COMMA));
    }
    consume(TOKEN_RPAREN, "Expect ')' after extern proc parameters.");

    // Optional: -> return_type
    char ret_buf[256] = "void";
    if (match(TOKEN_ARROW)) {
        int rlen = 0;
        while (!check(TOKEN_NEWLINE) && !check(TOKEN_EOF) &&
               !check(TOKEN_DEDENT)) {
            Token rtok = current_token;
            if (rtok.length + rlen < (int)sizeof(ret_buf)-1) {
                memcpy(ret_buf + rlen, rtok.start, rtok.length);
                rlen += rtok.length;
            }
            advance_parser();
        }
        ret_buf[rlen] = '\0';
    }
    match(TOKEN_NEWLINE);

    // Extract lib name from pragma_list if @clib("libname") is present
    char* lib_name = NULL;
    for (Pragma* p = pragma_list; p != NULL; p = p->next) {
        if (strcmp(p->name, "clib") == 0 && p->arg_count > 0) {
            lib_name = SAGE_STRDUP(p->args[0]);
            break;
        }
    }

    Stmt* s = SAGE_ALLOC(sizeof(Stmt));
    s->type = STMT_EXTERN;
    s->as.extern_proc.name = name;
    s->as.extern_proc.params = params;
    s->as.extern_proc.param_type_names = param_types;
    s->as.extern_proc.param_count = count;
    s->as.extern_proc.return_type_name = SAGE_STRDUP(ret_buf);
    s->as.extern_proc.lib_name = lib_name;
    s->next = NULL;
    s->pragmas = pragma_list;
    return s;
}

static Stmt* proc_declaration() {
    if (current_token.type == TOKEN_IDENTIFIER || current_token.type == TOKEN_INIT) {
        Token name = current_token;
        advance_parser();

        // Phase 17: optional generic type parameters [T, U]
        Token* type_params = NULL;
        int type_param_count = 0;
        parse_generic_type_params(&type_params, &type_param_count);

        consume(TOKEN_LPAREN, "Expect '(' after procedure name.");

        Token* params = NULL;
        TypeAnnotation** param_types = NULL;
        Expr** defaults = NULL;
        int count = 0;
        int capacity = 0;
        int required = 0;
        int seen_default = 0;

        if (!check(TOKEN_RPAREN)) {
            do {
                if (check(TOKEN_RPAREN)) break; // trailing comma
                if (current_token.type == TOKEN_SELF || current_token.type == TOKEN_IDENTIFIER) {
                    if (count >= capacity) {
                        capacity = capacity == 0 ? 4 : capacity * 2;
                        params = SAGE_REALLOC(params, sizeof(Token) * capacity);
                        param_types = SAGE_REALLOC(param_types, sizeof(TypeAnnotation*) * capacity);
                        defaults = SAGE_REALLOC(defaults, sizeof(Expr*) * capacity);
                    }
                    params[count] = current_token;
                    param_types[count] = NULL;
                    defaults[count] = NULL;
                    advance_parser();
                    // Optional type annotation: param: Type
                    if (match(TOKEN_COLON)) {
                        param_types[count] = parse_type_annotation();
                    }
                    if (match(TOKEN_ASSIGN)) {
                        defaults[count] = expression();
                        seen_default = 1;
                    } else {
                        if (seen_default) {
                            parser_report(current_token, token_span(&current_token),
                                          "non-default parameter follows default parameter",
                                          "parameters with defaults must come after required parameters");
                        }
                        required++;
                    }
                    count++;
                } else {
                    parser_report(current_token, token_span(&current_token),
                                  "expected parameter name",
                                  "parameters must be identifiers");
                    break;
                }
            } while (match(TOKEN_COMMA));
        }
        consume(TOKEN_RPAREN, "Expect ')' after parameters.");

        // Optional return type: -> Type
        TypeAnnotation* return_type = NULL;
        if (match(TOKEN_ARROW)) {
            return_type = parse_type_annotation();
        }

        consume(TOKEN_COLON, "Expect ':' after procedure signature.");
        consume(TOKEN_NEWLINE, "Expect newline before procedure body.");
        Stmt* body = block();

        Stmt* s = new_proc_stmt(name, params, count, body);
        s->as.proc.param_types = param_types;
        s->as.proc.defaults = defaults;
        s->as.proc.required_count = required;
        s->as.proc.return_type = return_type;
        s->as.proc.type_params = type_params;
        s->as.proc.type_param_count = type_param_count;
        s->as.proc.doc = take_pending_doc();
        return s;
    }
    
    parser_report(current_token, token_span(&current_token),
                  "expected procedure name",
                  "write a name after 'proc', for example: proc greet(name):");
    return NULL;
}

static Stmt* async_proc_declaration() {
    consume(TOKEN_PROC, "Expect 'proc' after 'async'.");
    if (current_token.type == TOKEN_IDENTIFIER) {
        Token name = current_token;
        advance_parser();

        consume(TOKEN_LPAREN, "Expect '(' after procedure name.");

        Token* params = NULL;
        int count = 0;
        int capacity = 0;

        if (!check(TOKEN_RPAREN)) {
            do {
                if (check(TOKEN_RPAREN)) break; // trailing comma
                if (current_token.type == TOKEN_SELF || current_token.type == TOKEN_IDENTIFIER) {
                    if (count >= capacity) {
                        capacity = capacity == 0 ? 4 : capacity * 2;
                        params = SAGE_REALLOC(params, sizeof(Token) * capacity);
                    }
                    params[count++] = current_token;
                    advance_parser();
                } else {
                    parser_report(current_token, token_span(&current_token),
                                  "expected parameter name",
                                  "parameters must be identifiers");
                    break;
                }
            } while (match(TOKEN_COMMA));
        }
        consume(TOKEN_RPAREN, "Expect ')' after parameters.");
        consume(TOKEN_COLON, "Expect ':' after procedure signature.");
        consume(TOKEN_NEWLINE, "Expect newline before procedure body.");
        Stmt* body = block();

        return new_async_proc_stmt(name, params, count, body);
    }

    parser_report(current_token, token_span(&current_token),
                  "expected procedure name after 'async proc'",
                  "write a name after 'async proc', for example: async proc fetch():");
    return NULL;
}

static Stmt* class_declaration() {
    consume(TOKEN_IDENTIFIER, "Expect class name.");
    Token name = previous_token;
    
    Token parent;
    int has_parent = 0;
    if (match(TOKEN_LPAREN)) {
        consume(TOKEN_IDENTIFIER, "Expect parent class name.");
        parent = previous_token;
        consume(TOKEN_RPAREN, "Expect ')' after parent class.");
        has_parent = 1;
    }
    
    consume(TOKEN_COLON, "Expect ':' after class header.");
    consume(TOKEN_NEWLINE, "Expect newline after class header.");
    consume(TOKEN_INDENT, "Expect indentation in class body.");
    
    Stmt* method_head = NULL;
    Stmt* method_current = NULL;
    
    while (!check(TOKEN_DEDENT) && !check(TOKEN_EOF)) {
        if (match(TOKEN_NEWLINE)) {
            continue;
        }
        
        if (match(TOKEN_PROC)) {
            Stmt* method = proc_declaration();
            
            if (method_head == NULL) {
                method_head = method;
                method_current = method;
            } else {
                method_current->next = method;
                method_current = method;
            }
        } else if (match(TOKEN_ASYNC)) {
            Stmt* method = async_proc_declaration();
            
            if (method_head == NULL) {
                method_head = method;
                method_current = method;
            } else {
                method_current->next = method;
                method_current = method;
            }
        } else {
            parser_report(current_token, token_span(&current_token),
                          "only methods are allowed in a class body",
                          "use 'proc' or 'async proc' to define methods inside a class");
        }
    }
    
    consume(TOKEN_DEDENT, "Expect dedent after class body.");
    
    return new_class_stmt(name, parent, has_parent, method_head);
}

// struct Point:
//     x: Int
//     y: Int
static Stmt* struct_declaration() {
    consume(TOKEN_IDENTIFIER, "Expect struct name.");
    Token name = previous_token;

    // Phase 17: optional generic type parameters [T, U]
    Token* type_params = NULL;
    int type_param_count = 0;
    parse_generic_type_params(&type_params, &type_param_count);

    consume(TOKEN_COLON, "Expect ':' after struct name.");
    consume(TOKEN_NEWLINE, "Expect newline after struct header.");
    consume(TOKEN_INDENT, "Expect indentation in struct body.");

    Token* field_names = NULL;
    TypeAnnotation** field_types = NULL;
    int count = 0;
    int capacity = 0;

    while (!check(TOKEN_DEDENT) && !check(TOKEN_EOF)) {
        if (match(TOKEN_NEWLINE)) continue;
        consume(TOKEN_IDENTIFIER, "Expect field name in struct.");
        Token fname = previous_token;
        TypeAnnotation* ftype = NULL;
        if (match(TOKEN_COLON)) {
            ftype = parse_type_annotation();
        }
        if (count >= capacity) {
            capacity = capacity == 0 ? 4 : capacity * 2;
            field_names = SAGE_REALLOC(field_names, sizeof(Token) * capacity);
            field_types = SAGE_REALLOC(field_types, sizeof(TypeAnnotation*) * capacity);
        }
        field_names[count] = fname;
        field_types[count] = ftype;
        count++;
        match(TOKEN_NEWLINE);
    }
    consume(TOKEN_DEDENT, "Expect dedent after struct body.");
    Stmt* s = new_struct_stmt(name, field_names, field_types, count);
    s->as.struct_stmt.type_params = type_params;
    s->as.struct_stmt.type_param_count = type_param_count;
    return s;
}

// enum Color:
//     Red
//     Green
//     Blue
static Stmt* enum_declaration() {
    consume(TOKEN_IDENTIFIER, "Expect enum name.");
    Token name = previous_token;
    consume(TOKEN_COLON, "Expect ':' after enum name.");
    consume(TOKEN_NEWLINE, "Expect newline after enum header.");
    consume(TOKEN_INDENT, "Expect indentation in enum body.");

    Token* variants = NULL;
    Token** variant_fields = NULL;
    int* variant_field_counts = NULL;
    int count = 0;
    int capacity = 0;

    while (!check(TOKEN_DEDENT) && !check(TOKEN_EOF)) {
        if (match(TOKEN_NEWLINE)) continue;
        consume(TOKEN_IDENTIFIER, "Expect variant name in enum.");
        Token vname = previous_token;
        if (count >= capacity) {
            capacity = capacity == 0 ? 4 : capacity * 2;
            variants = SAGE_REALLOC(variants, sizeof(Token) * capacity);
            variant_fields = SAGE_REALLOC(variant_fields, sizeof(Token*) * capacity);
            variant_field_counts = SAGE_REALLOC(variant_field_counts, sizeof(int) * capacity);
        }
        variants[count] = vname;
        variant_fields[count] = NULL;
        variant_field_counts[count] = 0;

        // P3: Check for (field1: type1, field2: type2) or (field1, field2)
        if (match(TOKEN_LPAREN)) {
            Token* fields = NULL;
            int fc = 0, fcap = 0;
            while (!check(TOKEN_RPAREN) && !check(TOKEN_EOF)) {
                consume(TOKEN_IDENTIFIER, "Expect field name in enum variant.");
                Token fname = previous_token;
                // Optional type annotation (skip it for now)
                if (match(TOKEN_COLON)) {
                    // Consume type annotation — just skip it
                    if (check(TOKEN_IDENTIFIER)) advance_parser();
                }
                if (fc >= fcap) {
                    fcap = fcap == 0 ? 4 : fcap * 2;
                    fields = SAGE_REALLOC(fields, sizeof(Token) * fcap);
                }
                fields[fc++] = fname;
                if (!match(TOKEN_COMMA)) break;
            }
            consume(TOKEN_RPAREN, "Expect ')' after enum variant fields.");
            variant_fields[count] = fields;
            variant_field_counts[count] = fc;
        }

        count++;
        match(TOKEN_NEWLINE);
    }
    consume(TOKEN_DEDENT, "Expect dedent after enum body.");
    Stmt* s = new_enum_stmt(name, variants, count);
    s->as.enum_stmt.variant_fields = variant_fields;
    s->as.enum_stmt.variant_field_counts = variant_field_counts;
    return s;
}

// trait Printable:
//     proc to_string(self) -> String
static Stmt* trait_declaration() {
    consume(TOKEN_IDENTIFIER, "Expect trait name.");
    Token name = previous_token;
    consume(TOKEN_COLON, "Expect ':' after trait name.");
    consume(TOKEN_NEWLINE, "Expect newline after trait header.");
    consume(TOKEN_INDENT, "Expect indentation in trait body.");

    Stmt* method_head = NULL;
    Stmt* method_current = NULL;

    while (!check(TOKEN_DEDENT) && !check(TOKEN_EOF)) {
        if (match(TOKEN_NEWLINE)) continue;
        if (match(TOKEN_PROC)) {
            Stmt* method = proc_declaration();
            if (method_head == NULL) {
                method_head = method;
                method_current = method;
            } else {
                method_current->next = method;
                method_current = method;
            }
        } else {
            parser_report(current_token, token_span(&current_token),
                          "only method signatures allowed in trait body",
                          "use 'proc' to define method signatures");
            break;
        }
    }
    consume(TOKEN_DEDENT, "Expect dedent after trait body.");
    return new_trait_stmt(name, method_head);
}

static Stmt* statement() {
    if (match(TOKEN_PRINT)) return print_statement();
    if (match(TOKEN_IF)) return if_statement();
    if (match(TOKEN_WHILE)) return while_statement();
    if (match(TOKEN_FOR)) return for_statement();
    if (match(TOKEN_TRY)) return try_statement();
    if (match(TOKEN_RAISE)) return raise_statement();
    if (match(TOKEN_YIELD)) return yield_statement();
    if (match(TOKEN_DEFER)) return defer_statement();
    if (match(TOKEN_MATCH)) return match_statement();
    // Phase 17: comptime block
    if (match(TOKEN_COMPTIME)) return comptime_statement();
    // Phase 1.8: unsafe block — executes as normal block (semantic marker)
    if (match(TOKEN_UNSAFE)) {
        consume(TOKEN_COLON, "Expect ':' after 'unsafe'.");
        consume(TOKEN_NEWLINE, "Expect newline after 'unsafe:'.");
        return block();
    }
    if (match(TOKEN_BREAK)) return new_break_stmt();
    if (match(TOKEN_CONTINUE)) return new_continue_stmt();
    // 'end' keyword: optional block terminator (blocks use INDENT/DEDENT)
    if (match(TOKEN_END)) return new_expr_stmt(new_nil_expr());

    Expr* expr = expression();
    
    // Handle assignment statements (x = value) where expr is a SET expression with NULL object
    if (expr->type == EXPR_SET && expr->as.set.object == NULL) {
        // This is a variable assignment, treat it as a statement
        return new_expr_stmt(expr);
    }
    
    return new_expr_stmt(expr);
}

static char* pending_doc = NULL;

static void collect_doc_comment(void) {
    // Collect consecutive ## lines into one doc string
    if (pending_doc) { free(pending_doc); pending_doc = NULL; }
    int total_len = 0;
    int capacity = 256;
    char* buf = SAGE_ALLOC(capacity);
    buf[0] = '\0';
    while (check(TOKEN_DOC_COMMENT)) {
        Token doc = current_token;
        advance_parser();
        if (total_len > 0) { buf[total_len++] = '\n'; }
        int needed = total_len + doc.length + 2;
        if (needed > capacity) { capacity = needed * 2; buf = SAGE_REALLOC(buf, capacity); }
        memcpy(buf + total_len, doc.start, doc.length);
        total_len += doc.length;
        buf[total_len] = '\0';
        match(TOKEN_NEWLINE);
    }
    if (total_len > 0) {
        pending_doc = buf;
    } else {
        free(buf);
    }
}

static char* take_pending_doc(void) {
    char* doc = pending_doc;
    pending_doc = NULL;
    return doc;
}

static Stmt* declaration() {
    while (match(TOKEN_NEWLINE));

    // Collect doc comments before declarations
    if (check(TOKEN_DOC_COMMENT)) {
        collect_doc_comment();
    }

    if (check(TOKEN_DEDENT) || check(TOKEN_EOF)) {
        return NULL;
    }

    // Phase 17: @pragma decorators — collect and attach to next declaration
    Pragma* pragma_list = NULL;
    while (check(TOKEN_AT)) {
        advance_parser(); // consume '@'

        // Peek at the identifier/keyword name after @
        // Note: 'unsafe' is TOKEN_UNSAFE (keyword), others are TOKEN_IDENTIFIER
        int is_annot_candidate = check(TOKEN_IDENTIFIER) || check(TOKEN_UNSAFE);
        if (is_annot_candidate) {
            // Check for memory/safety block annotations: @manual, @gc, @trusted, @unsafe, @owned
            int name_len = (int)current_token.length;
            const char* name_start = current_token.start;

            BlockAnnotation block_annot = BLOCK_ANNOT_NONE;
            if      (check(TOKEN_UNSAFE))                                         block_annot = BLOCK_ANNOT_UNSAFE;
            else if (name_len == 6 && strncmp(name_start, "manual",  6) == 0)    block_annot = BLOCK_ANNOT_MANUAL;
            else if (name_len == 2 && strncmp(name_start, "gc",      2) == 0)    block_annot = BLOCK_ANNOT_GC;
            else if (name_len == 7 && strncmp(name_start, "trusted", 7) == 0)    block_annot = BLOCK_ANNOT_TRUSTED;
            else if (name_len == 5 && strncmp(name_start, "owned",   5) == 0)    block_annot = BLOCK_ANNOT_OWNED;

            if (block_annot != BLOCK_ANNOT_NONE) {
                advance_parser(); // consume annotation name
                // Optional argument: @manual("note") — consume and ignore for now
                char annot_arg[64] = {0};
                if (match(TOKEN_LPAREN)) {
                    if (check(TOKEN_STRING)) {
                        int len = (int)current_token.length - 2;
                        if (len < 0) len = 0;
                        if (len > 63) len = 63;
                        strncpy(annot_arg, current_token.start + 1, (size_t)len);
                        annot_arg[len] = '\0';
                        advance_parser();
                    } else if (check(TOKEN_IDENTIFIER)) {
                        int len = (int)current_token.length;
                        if (len > 63) len = 63;
                        strncpy(annot_arg, current_token.start, (size_t)len);
                        annot_arg[len] = '\0';
                        advance_parser();
                    }
                    while (!check(TOKEN_RPAREN) && !check(TOKEN_EOF)) advance_parser();
                    match(TOKEN_RPAREN);
                }
                // Expect colon then indented block (uses the standard block() parser)
                if (!match(TOKEN_COLON)) {
                    // Single statement form: @unsafe <stmt>
                    while (match(TOKEN_NEWLINE));
                    Stmt* body = statement();
                    return new_annotated_block_stmt(body, block_annot, annot_arg);
                }
                // block() handles INDENT/DEDENT and calls declaration() for each stmt
                Stmt* body = block();
                return new_annotated_block_stmt(body, block_annot, annot_arg);
            }
        }

        // Not a block annotation — treat as regular @pragma
        Pragma* p = parse_pragma();
        p->next = pragma_list;
        pragma_list = p;
        match(TOKEN_NEWLINE);
        while (match(TOKEN_NEWLINE));
    }

    // Phase 17: macro definition
    if (match(TOKEN_MACRO)) {
        Stmt* s = macro_declaration();
        if (pragma_list) { s->pragmas = pragma_list; }
        return s;
    }

    if (match(TOKEN_CLASS)) {
        Stmt* s = class_declaration();
        if (pragma_list) { s->pragmas = pragma_list; }
        return s;
    }
    // struct/enum/trait/impl: only treat as declaration if followed by identifier (name)
    if (check(TOKEN_STRUCT) || check(TOKEN_ENUM) || check(TOKEN_TRAIT) || check(TOKEN_IMPL)) {
        LexerState saved_ls = lexer_get_state();
        Token saved_ct = current_token;
        Token saved_pt = previous_token;
        TokenType kw = current_token.type;
        advance_parser();
        if (current_token.type == TOKEN_IDENTIFIER) {
            Stmt* s = NULL;
            if (kw == TOKEN_STRUCT) s = struct_declaration();
            else if (kw == TOKEN_ENUM) s = enum_declaration();
            else if (kw == TOKEN_TRAIT) s = trait_declaration();
            else if (kw == TOKEN_IMPL) {
                // P3: impl TypeName: <methods>
                Token target = current_token;
                advance_parser(); // consume type name
                consume(TOKEN_COLON, "Expect ':' after impl type name.");
                consume(TOKEN_NEWLINE, "Expect newline after impl header.");
                consume(TOKEN_INDENT, "Expect indentation in impl body.");
                
                Stmt* method_head = NULL;
                Stmt* method_tail = NULL;
                while (!check(TOKEN_DEDENT) && !check(TOKEN_EOF)) {
                    if (match(TOKEN_NEWLINE)) continue;
                    if (match(TOKEN_PROC)) {
                        Stmt* method = proc_declaration();
                        if (method_head == NULL) {
                            method_head = method;
                            method_tail = method;
                        } else {
                            method_tail->next = method;
                            method_tail = method;
                        }
                    } else {
                        parser_report(current_token, token_span(&current_token),
                                      "only proc definitions allowed in impl block",
                                      "use 'proc' to define methods");
                        break;
                    }
                }
                consume(TOKEN_DEDENT, "Expect dedent after impl body.");
                
                s = SAGE_ALLOC(sizeof(Stmt));
                s->type = STMT_IMPL;
                s->as.impl_stmt.target = target;
                s->as.impl_stmt.methods = method_head;
                s->next = NULL;
                s->pragmas = NULL;
            }
            if (s && pragma_list) { s->pragmas = pragma_list; }
            if (s) return s;
        }
        lexer_set_state(saved_ls);
        current_token = saved_ct;
        previous_token = saved_pt;
    }
    if (match(TOKEN_SPAWN)) {
        // spawn: <block>  — runs body in a new thread
        consume(TOKEN_COLON, "Expect ':' after 'spawn'.");
        Stmt* body = block();
        Stmt* s = SAGE_ALLOC(sizeof(Stmt));
        s->type = STMT_SPAWN;
        s->as.spawn_stmt.body = body;
        s->next = NULL;
        s->pragmas = pragma_list;
        return s;
    }

    if (match(TOKEN_ASYNC)) {
        Stmt* s = async_proc_declaration();
        if (pragma_list) { s->pragmas = pragma_list; }
        return s;
    }
    if (match(TOKEN_PROC)) {
        Stmt* s = proc_declaration();
        if (pragma_list) { s->pragmas = pragma_list; }
        return s;
    }

    // Phase 4: extern proc declaration for @clib bindings
    // Syntax: extern proc name(arg: type, ...) -> ret_type
    if (check(TOKEN_IDENTIFIER) &&
        current_token.length == 6 &&
        strncmp(current_token.start, "extern", 6) == 0) {
        advance_parser(); // consume 'extern'
        if (match(TOKEN_PROC)) {
            Stmt* s = extern_proc_declaration(pragma_list);
            return s;
        }
        // Not 'extern proc' — back off and fall through
        // (handled later as expression)
    }
    
    // PHASE 8: Import statements
    if (match(TOKEN_IMPORT) || check(TOKEN_FROM)) {
        Stmt* stmt = import_statement();
        match(TOKEN_NEWLINE);
        return stmt;
    }

    if (match(TOKEN_RETURN)) {
        Expr* value = NULL;
        if (!check(TOKEN_NEWLINE)) {
            value = expression();
        }
        match(TOKEN_NEWLINE);
        return new_return_stmt(value);
    }

    // P2: Type-first declaration syntax: int x = 12, str name = "Abi", etc.
    // These are contextual — int/str/float/bool are identifiers, not keywords.
    // We detect: <type-name-identifier> <variable-name-identifier> =
    if (current_token.type == TOKEN_IDENTIFIER) {
        const char* t = current_token.start;
        int tl = current_token.length;
        int is_type_name =
            (tl == 3 && strncmp(t, "int", 3) == 0) ||
            (tl == 5 && strncmp(t, "float", 5) == 0) ||
            (tl == 6 && strncmp(t, "double", 6) == 0) ||
            (tl == 3 && strncmp(t, "str", 3) == 0) ||
            (tl == 4 && strncmp(t, "bool", 4) == 0) ||
            (tl == 3 && strncmp(t, "num", 3) == 0) ||
            (tl == 4 && strncmp(t, "byte", 4) == 0) ||
            (tl == 3 && strncmp(t, "any", 3) == 0) ||
            (tl == 5 && strncmp(t, "Array", 5) == 0) ||
            (tl == 4 && strncmp(t, "Dict", 4) == 0) ||
            (tl == 5 && strncmp(t, "Tuple", 5) == 0);
        if (is_type_name) {
            // Peek ahead: type name followed by identifier followed by = ?
            LexerState saved = lexer_get_state();
            Token saved_current = current_token;
            Token saved_prev = previous_token;
            Token type_token = current_token;
            advance_parser(); // consume type name
            if (current_token.type == TOKEN_IDENTIFIER) {
                Token var_name = current_token;
                // Peek one more: is there = or newline?
                LexerState saved2 = lexer_get_state();
                Token saved_current2 = current_token;
                advance_parser(); // consume variable name
                if (current_token.type == TOKEN_ASSIGN || current_token.type == TOKEN_NEWLINE ||
                    current_token.type == TOKEN_EOF) {
                    // Confirmed: this is a type-first declaration
                    TypeAnnotation* type_ann = new_type_annotation(type_token, NULL, 0, 0);
                    Expr* initializer = NULL;
                    if (match(TOKEN_ASSIGN)) {
                        initializer = expression();
                    }
                    Stmt* stmt = new_let_stmt(var_name, initializer);
                    stmt->as.let.type_ann = type_ann;
                    stmt->as.let.is_mutable = 1;  // P4: type-first decls are mutable (C++ style)
                    if (pragma_list) { stmt->pragmas = pragma_list; }
                    match(TOKEN_NEWLINE);
                    return stmt;
                }
                // Not a declaration — could be `int(3.14)` or `str + "x"` etc
                lexer_set_state(saved);
                current_token = saved_current;
                previous_token = saved_prev;
            } else {
                lexer_set_state(saved);
                current_token = saved_current;
                previous_token = saved_prev;
            }
        }
    }

    if (match(TOKEN_LET) || match(TOKEN_VAR) || match(TOKEN_CONST)) {
        int is_var = (previous_token.type == TOKEN_VAR);
        int is_mutable = is_var;  // P4: var is mutable, let/const are immutable

        // Array destructuring: let [a, b, c] = expr
        if (check(TOKEN_LBRACKET)) {
            advance_parser(); // consume '['
            Token* names = NULL;
            int name_count = 0, name_cap = 0;
            int has_rest = 0;
            Token rest_name = {0};
            while (!check(TOKEN_RBRACKET) && !check(TOKEN_EOF)) {
                if (match(TOKEN_DOTDOT) || match(TOKEN_DOTDOTEQ)) {
                    if (match(TOKEN_IDENTIFIER)) rest_name = previous_token;
                    has_rest = 1;
                    break;
                }
                if (!match_identifier_like()) break;
                if (name_count >= name_cap) {
                    name_cap = name_cap == 0 ? 4 : name_cap * 2;
                    names = SAGE_REALLOC(names, sizeof(Token) * name_cap);
                }
                names[name_count++] = previous_token;
                if (!match(TOKEN_COMMA)) break;
            }
            consume(TOKEN_RBRACKET, "Expect ']' after destructure pattern.");
            TypeAnnotation* type_ann = NULL;
            if (match(TOKEN_COLON)) {
                if (check(TOKEN_IDENTIFIER)) parse_type_annotation();  // discard for now
            }
            consume(TOKEN_ASSIGN, "Expect '=' in destructuring let.");
            Expr* init = expression();
            match(TOKEN_NEWLINE);

            // Build a block: let __d = init; let a = __d[0]; let b = __d[1]; ...
            static int destruct_id = 0;
            char tmpname[32];
            snprintf(tmpname, sizeof(tmpname), "__destruct_%d", destruct_id++);
            // Emit as a let __d = init followed by let a = __d[0], let b = __d[1], ...
            Token tmp_tok;
            tmp_tok.type   = TOKEN_IDENTIFIER;
            tmp_tok.start  = SAGE_STRDUP(tmpname);
            tmp_tok.length = (int)strlen(tmpname);

            Stmt* head = new_let_stmt(tmp_tok, init);
            Stmt* tail = head;
            for (int idx = 0; idx < name_count; idx++) {
                Expr* var   = new_variable_expr(tmp_tok);
                Expr* index = new_number_expr((double)idx);
                Expr* elem  = new_index_expr(var, index);
                Stmt* s = new_let_stmt(names[idx], elem);
                tail->next = s;
                tail = s;
            }
            if (has_rest && rest_name.start) {
                Expr* var = new_variable_expr(tmp_tok);
                Stmt* s = new_let_stmt(rest_name, var);
                tail->next = s;
                tail = s;
            }
            return new_block_stmt(head);
        }

        // Dict destructuring: let {x, y} = expr
        if (check(TOKEN_LBRACE)) {
            advance_parser(); // consume '{'
            Token* names = NULL;
            int name_count = 0, name_cap = 0;
            while (!check(TOKEN_RBRACE) && !check(TOKEN_EOF)) {
                if (!match_identifier_like()) break;
                if (name_count >= name_cap) {
                    name_cap = name_cap == 0 ? 4 : name_cap * 2;
                    names = SAGE_REALLOC(names, sizeof(Token) * name_cap);
                }
                names[name_count++] = previous_token;
                if (!match(TOKEN_COMMA)) break;
            }
            consume(TOKEN_RBRACE, "Expect '}' after destructure pattern.");
            consume(TOKEN_ASSIGN, "Expect '=' in destructuring let.");
            Expr* init = expression();
            match(TOKEN_NEWLINE);

            static int destruct_did = 0;
            char tmpname2[32];
            snprintf(tmpname2, sizeof(tmpname2), "__destructd_%d", destruct_did++);
            Token tmp_tok2;
            tmp_tok2.type   = TOKEN_IDENTIFIER;
            tmp_tok2.start  = SAGE_STRDUP(tmpname2);
            tmp_tok2.length = (int)strlen(tmpname2);

            Stmt* head = new_let_stmt(tmp_tok2, init);
            Stmt* tail = head;
            for (int idx = 0; idx < name_count; idx++) {
                Expr* var  = new_variable_expr(tmp_tok2);
                char* key  = SAGE_ALLOC(names[idx].length + 1);
                memcpy(key, names[idx].start, names[idx].length);
                key[names[idx].length] = '\0';
                Expr* kstr = new_string_expr(key);
                Expr* elem = new_index_expr(var, kstr);
                Stmt* s    = new_let_stmt(names[idx], elem);
                tail->next = s;
                tail = s;
            }
            return new_block_stmt(head);
        }

        if (!match_identifier_like()) {
            parser_expected_error(current_token, TOKEN_IDENTIFIER, "Expect variable name.");
        }
        Token name = previous_token;

        // Optional type annotation: let x: Int = ...
        TypeAnnotation* type_ann = NULL;
        if (check(TOKEN_COLON)) {
            // Save state to peek ahead
            LexerState saved = lexer_get_state();
            Token saved_current = current_token;
            Token saved_prev = previous_token;
            advance_parser(); // consume :
            if (current_token.type == TOKEN_IDENTIFIER) {
                type_ann = parse_type_annotation();
            } else {
                // Not a type annotation, restore
                lexer_set_state(saved);
                current_token = saved_current;
                previous_token = saved_prev;
            }
        }

        Expr* initializer = NULL;
        if (match(TOKEN_ASSIGN)) {
            initializer = expression();
        }

        Stmt* stmt = new_let_stmt(name, initializer);
        stmt->as.let.type_ann = type_ann;
        stmt->as.let.is_mutable = is_mutable;  // P4: var = mutable, let/const = immutable
        if (pragma_list) { stmt->pragmas = pragma_list; }
        match(TOKEN_NEWLINE);
        return stmt;
    }

    Stmt* stmt = statement();
    if (pragma_list) { stmt->pragmas = pragma_list; }
    match(TOKEN_NEWLINE);
    return stmt;
}

Stmt* parse(void) {
    while (current_token.type == TOKEN_NEWLINE) {
        advance_parser();
    }

    if (current_token.type == TOKEN_EOF) return NULL;
    return declaration();
}

// Public expression parser for sub-parsers (e.g., string interpolation)
Expr* parse_expression_public(void) { return expression(); }
