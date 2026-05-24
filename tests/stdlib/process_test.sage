gc_disable()
# EXPECT: true
# EXPECT: sage
# EXPECT: test
# EXPECT: true

import std.process

# Platform check
var p = process.platform()
println(p.length > 0)

# Path utilities
println(process.extension("test.sage"))
println(process.basename("/home/user/test"))

# Exit codes
println(process.EXIT_SUCCESS == 0)
