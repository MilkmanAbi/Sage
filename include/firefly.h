// firefly.h -- Firefly: Error Diagnosis Subsystem for SageTree
//
// Every error in SageTree flows through Firefly. Parse errors, type
// errors, runtime errors, warnings. Firefly catches it, explains it,
// shows you where, and helps you fix it.
//
// Three verbosity levels: --firefly=full | minimal | off
//
// =========================================================================
// ERROR CODE REGISTRY
// =========================================================================
//
// E001  Undefined variable
// E002  Undefined function
// E003  Undefined module
// E004  Undefined type
// E005  Variable used before declaration
// E006  Name shadows builtin
// E007  Redeclared variable in same scope
// E008  (reserved)
// E009  (reserved)
//
// E010  Type mismatch (general)
// E011  Parameter type mismatch
// E012  Return type mismatch
// E013  Assignment type mismatch (typed variable)
// E014  Cannot convert between types
// E015  Invalid type annotation
// E016  Nil assigned to non-optional type
// E017  (reserved)
// E018  (reserved)
// E019  (reserved)
//
// E020  Array index out of bounds
// E021  String index out of bounds
// E022  Tuple index out of bounds
// E023  Negative index exceeds length
// E024  Slice range invalid
// E025  Dict key not found
// E026  (reserved)
// E027  (reserved)
// E028  (reserved)
// E029  (reserved)
//
// E030  Division by zero
// E031  Modulo by zero
// E032  Integer overflow
// E033  Float NaN result
// E034  Bitwise operation on non-integer
// E035  (reserved)
// E036  (reserved)
// E037  (reserved)
// E038  (reserved)
// E039  (reserved)
//
// E040  Cannot reassign immutable variable (let)
// E041  Cannot reassign constant (const)
// E042  Cannot mutate borrowed value
// E043  Cannot move out of borrowed context
// E044  (reserved)
// E045  (reserved)
// E046  (reserved)
// E047  (reserved)
// E048  (reserved)
// E049  (reserved)
//
// E050  No such method on type
// E051  No such property on type
// E052  Cannot call non-callable value
// E053  Struct has no field
// E054  Enum has no variant
// E055  (reserved)
// E056  (reserved)
// E057  (reserved)
// E058  (reserved)
// E059  (reserved)
//
// E060  Wrong number of arguments
// E061  Too many arguments (max 255)
// E062  Missing required argument
// E063  Unexpected named argument
// E064  Duplicate named argument
// E065  (reserved)
// E066  (reserved)
// E067  (reserved)
// E068  (reserved)
// E069  (reserved)
//
// E070  Module not found
// E071  Circular import detected
// E072  Maximum recursion depth exceeded
// E073  Maximum loop iterations exceeded
// E074  Import failed
// E075  (reserved)
// E076  (reserved)
// E077  (reserved)
// E078  (reserved)
// E079  (reserved)
//
// E080  Double free detected
// E081  Use after free
// E082  Memory leak (unfreed allocation in @manual)
// E083  Null pointer dereference
// E084  Buffer overrun
// E085  Use after move
// E086  Double move
// E087  (reserved)
// E088  (reserved)
// E089  (reserved)
//
// E090  Sandbox permission denied (filesystem read)
// E091  Sandbox permission denied (filesystem write)
// E092  Sandbox permission denied (network)
// E093  Sandbox permission denied (process exec)
// E094  Sandbox permission denied (FFI)
// E095  Sandbox permission denied (env read)
// E096  Sandbox rate limit exceeded
// E097  Sandbox resource limit exceeded
// E098  (reserved)
// E099  (reserved)
//
// E100  Unsupported operand types for + 
// E101  Unsupported operand types for -
// E102  Unsupported operand types for *
// E103  Unsupported operand types for /
// E104  Unsupported operand types for %
// E105  Cannot compare types
// E106  Cannot negate type
// E107  (reserved)
// E108  (reserved)
// E109  (reserved)
//
// W001  Unused variable
// W002  Non-exhaustive match (no default)
// W003  String concatenation in loop (use .join())
// W004  Shadowed variable
// W005  Missing return in typed function
// W006  Unreachable code after return
// W007  Comparison always true/false
// W008  Division result truncated (int / int)
// W009  (reserved)
// W010  (reserved)
//
// =========================================================================

#pragma once
#include "value.h"
#include "ast.h"
#include "env.h"
#include <stdarg.h>

typedef enum {
    FIREFLY_ERROR,
    FIREFLY_WARNING,
    FIREFLY_NOTE,
    FIREFLY_HINT,
} FireflySeverity;

typedef enum {
    FIREFLY_FULL,
    FIREFLY_MINIMAL,
    FIREFLY_OFF,
} FireflyVerbosity;

typedef enum {
    FIREFLY_PHASE_PARSE,
    FIREFLY_PHASE_CHECK,
    FIREFLY_PHASE_RUNTIME,
    FIREFLY_PHASE_LINK,
} FireflyPhase;

typedef struct {
    const char* filename;
    int line;
    int column;
    const char* line_start;
    int span;
} FireflyLoc;

typedef struct FireflyFrame {
    const char* function_name;
    FireflyLoc loc;
    struct FireflyFrame* next;
} FireflyFrame;

typedef struct {
    FireflyFrame* call_stack;
    int stack_depth;
    const char* current_file;
    int error_count;
    int warning_count;
    int max_errors;
    int suppress_after_max;
    FireflyVerbosity verbosity;
} FireflyContext;

// Lifecycle
void firefly_init(void);
void firefly_shutdown(void);
void firefly_set_file(const char* filename);
void firefly_set_verbosity(FireflyVerbosity v);
void firefly_set_code(const char* code);

// Call stack
void firefly_push_frame(const char* func_name, FireflyLoc loc);
void firefly_pop_frame(void);
int  ff_ctx_stack_depth(void);

// Location extraction
FireflyLoc firefly_loc_from_expr(Expr* expr);
FireflyLoc firefly_loc_from_token(Token* tok);

// Core reporting
void firefly_report(FireflySeverity severity, FireflyLoc loc, const char* fmt, ...);
void firefly_explain(const char* fmt, ...);
void firefly_advice(const char* fmt, ...);
void firefly_help(const char* fmt, ...);
void firefly_note(FireflyLoc loc, const char* fmt, ...);
void firefly_end(void);

// Convenience diagnostics
void firefly_undefined_var(Expr* expr, const char* name, int name_len, Env* env);
void firefly_type_error(Expr* expr, const char* expected, const char* got, const char* help_fmt, ...);
void firefly_index_error(Expr* expr, int index, int length, const char* type_name);
void firefly_div_zero(Expr* expr);
void firefly_immutable_error(Expr* expr, const char* name, int name_len);
void firefly_no_method(Expr* expr, const char* type_name, const char* method, int method_len);
void firefly_no_property(Expr* expr, const char* type_name, const char* prop, int prop_len);
void firefly_arity_error(Expr* expr, const char* func_name, int expected, int got);
void firefly_print_stack(void);
const char* firefly_suggest_name(const char* name, int name_len, Env* env);

// Warning pass
void firefly_warn_pass(Stmt* program, const char* filename);
