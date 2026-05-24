# EXPECT: hello	world
# EXPECT: line1
# EXPECT: line2
# EXPECT: she said "hi"
# EXPECT: back\slash
# EXPECT: A
# Tab escape
println("hello\tworld")
# Newline escape
println("line1\nline2")
# Quote escape
println("she said \"hi\"")
# Backslash escape
println("back\\slash")
# Hex escape
println("\x41")
