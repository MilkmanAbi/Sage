gc_disable()
# Structured logging framework
# Levels: TRACE, DEBUG, INFO, WARN, ERROR, FATAL

# Log levels
let TRACE = 0
let DEBUG = 1
let INFO = 2
let WARN = 3
let ERROR = 4
let FATAL = 5

let LEVEL_NAMES = ["TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL"]

proc level_name(level):
    if level >= 0 and level <= 5:
        return LEVEL_NAMES[level]
    return "UNKNOWN"

# Create a logger
proc create(name, min_level):
    let logger = {}
    logger["name"] = name
    logger["level"] = min_level
    logger["handlers"] = []
    logger["fields"] = {}
    return logger

# Default console handler
proc console_handler(entry):
    var msg = "[" + entry["level_name"] + "] "
    if entry["name"].length > 0:
        msg = msg + entry["name"] + ": "
    msg = msg + entry["message"]
    let keys = dict_keys(entry["fields"])
    if keys.length > 0:
        msg = msg + " {"
        for i in 0..keys.length:
            if i > 0:
                msg = msg + ", "
            msg = msg + keys[i] + "=" + str(entry["fields"][keys[i]])
        msg = msg + "}"
    println(msg)

# Add a handler to the logger
proc add_handler(logger, handler):
    logger["handlers"].push(handler)
    return logger

# Add a default field that appears on every log entry
proc with_field(logger, key, value):
    logger["fields"][key] = value
    return logger

# Create a child logger with inherited fields
proc child(logger, name):
    let c = create(name, logger["level"])
    let keys = dict_keys(logger["fields"])
    for i in 0..keys.length:
        c["fields"][keys[i]] = logger["fields"][keys[i]]
    let handlers = logger["handlers"]
    for i in 0..handlers.length:
        c["handlers"].push(handlers[i])
    return c

# Core log function
proc log_entry(logger, level, message, extra_fields):
    if level < logger["level"]:
        return
    let entry = {}
    entry["level"] = level
    entry["level_name"] = level_name(level)
    entry["name"] = logger["name"]
    entry["message"] = message
    # Merge logger fields + extra fields
    let fields = {}
    let lkeys = dict_keys(logger["fields"])
    for i in 0..lkeys.length:
        fields[lkeys[i]] = logger["fields"][lkeys[i]]
    if extra_fields != nil:
        let ekeys = dict_keys(extra_fields)
        for i in 0..ekeys.length:
            fields[ekeys[i]] = extra_fields[ekeys[i]]
    entry["fields"] = fields
    # Dispatch to handlers
    let handlers = logger["handlers"]
    if handlers.length == 0:
        console_handler(entry)
    else:
        for i in 0..handlers.length:
            handlers[i](entry)

# Convenience methods
proc trace(logger, message):
    log_entry(logger, 0, message, nil)

proc debug(logger, message):
    log_entry(logger, 1, message, nil)

proc info(logger, message):
    log_entry(logger, 2, message, nil)

proc warn(logger, message):
    log_entry(logger, 3, message, nil)

proc error(logger, message):
    log_entry(logger, 4, message, nil)

proc fatal(logger, message):
    log_entry(logger, 5, message, nil)

# Log with extra fields
proc info_f(logger, message, fields):
    log_entry(logger, 2, message, fields)

proc error_f(logger, message, fields):
    log_entry(logger, 4, message, fields)

# Create a default logger
proc default_logger():
    return create("", 2)

# Set minimum log level
proc set_level(logger, level):
    logger["level"] = level
