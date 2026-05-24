# EXPECT: HELLO
# EXPECT: hello
# EXPECT: trim me
# EXPECT: [hello, world]
# EXPECT: true
# EXPECT: false
# EXPECT: true
# EXPECT: 5
# EXPECT: hxllo
var s = "hello"
println(s.upper())
println("HELLO".lower())
println("  trim me  ".trim())
println("hello world".split(" "))
println("hello world".contains("world"))
println("hello world".contains("xyz"))
println("hello".starts_with("hel"))
println(s.length)
println(s.replace("e", "x"))
