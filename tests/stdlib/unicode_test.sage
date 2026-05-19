gc_disable()
# EXPECT: HELLO
# EXPECT: hello
# EXPECT: Hello World
# EXPECT: hello
# EXPECT: true
# EXPECT: true

import std.unicode

println(unicode.to_upper("hello"))
println(unicode.to_lower("HELLO"))
println(unicode.to_title("hello world"))
println(unicode.trim("  hello  "))
println(unicode.starts_with("hello world", "hello"))
println(unicode.ends_with("hello world", "world"))
