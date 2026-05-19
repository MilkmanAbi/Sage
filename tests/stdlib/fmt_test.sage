gc_disable()
# EXPECT: 0x00ff
# EXPECT: 1.0 MB
# EXPECT: Hello, World!
# EXPECT: 1, 2, 3
# EXPECT: 42.50%

import std.fmt

println(fmt.to_hex(255, 4))
println(fmt.format_bytes(1572864))
println(fmt.template("Hello, {name}!", {"name": "World"}))
println(fmt.join([1, 2, 3], ", "))
println(fmt.format_pct(0.425, 2))
