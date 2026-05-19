gc_disable()
# Process management and environment utilities
# Wraps sys module with higher-level abstractions

import sys

# ============================================================================
# Environment variables
# ============================================================================

proc get_env(name):
    return sys.getenv(name)

proc get_env_or(name, default_val):
    let val = sys.getenv(name)
    if val == nil:
        return default_val
    return val

# ============================================================================
# Process info
# ============================================================================

proc platform():
    return sys.platform

proc version():
    return sys.version

proc args():
    return sys.args

# ============================================================================
# Exit codes
# ============================================================================

let EXIT_SUCCESS = 0
let EXIT_FAILURE = 1
let EXIT_USAGE = 64
let EXIT_DATA_ERR = 65
let EXIT_NO_INPUT = 66
let EXIT_SOFTWARE = 70
let EXIT_OS_ERR = 71
let EXIT_IO_ERR = 74
let EXIT_CONFIG = 78

proc exit_with(code):
    sys.exit(code)

proc exit_ok():
    sys.exit(0)

proc exit_error(message):
    println(message)
    sys.exit(1)

# ============================================================================
# Path utilities
# ============================================================================

proc path_separator():
    let p = platform()
    if p == "windows":
        return chr(92)
    return "/"

proc join_path(parts):
    let sep = path_separator()
    var result = ""
    for i in 0..parts.length:
        if i > 0:
            result = result + sep
        result = result + parts[i]
    return result

proc basename(path):
    let sep = path_separator()
    var last = 0
    for i in 0..path.length:
        if path[i] == sep or path[i] == "/":
            last = i + 1
    var result = ""
    for i in 0..path.length - last:
        result = result + path[last + i]
    return result

proc dirname(path):
    let sep = path_separator()
    var last = 0
    for i in 0..path.length:
        if path[i] == sep or path[i] == "/":
            last = i
    var result = ""
    for i in 0..last:
        result = result + path[i]
    if result.length == 0:
        return "."
    return result

proc extension(path):
    let name = basename(path)
    var dot = -1
    for i in 0..name.length:
        if name[i] == ".":
            dot = i
    if dot < 1:
        return ""
    var ext = ""
    for i in 0..name.length - dot - 1:
        ext = ext + name[dot + 1 + i]
    return ext

# ============================================================================
# Timer
# ============================================================================

proc timer_start():
    return clock()

proc timer_elapsed(start_time):
    return clock() - start_time

proc timer_elapsed_ms(start_time):
    return (clock() - start_time) * 1000
