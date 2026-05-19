gc_disable()
# EXPECT: true
# EXPECT: true
# EXPECT: false
# EXPECT: 123
# EXPECT: h-llo
# EXPECT: 3

import std.regex

# Basic match
println(regex.test("hello", "say hello world"))
println(regex.full_match("abc", "abc"))
println(regex.full_match("abc", "abcd"))

# Digit match
var m = regex.search("[0-9]+", "abc123def")
println(m["text"])

# Replace
println(regex.replace_first("e", "hello", "-"))

# Find all
var matches = regex.find_all("[a-z]+", "hello world foo")
println(matches.length)
