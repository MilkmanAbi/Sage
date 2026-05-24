gc_disable()
# EXPECT: INFO
# EXPECT: true
# EXPECT: true

import std.log

println(log.level_name(2))

var logger = log.create("test", 2)
println(logger["level"] == 2)

# Verify handler infrastructure
log.add_handler(logger, log.console_handler)
println(logger["handlers"].length == 1)
