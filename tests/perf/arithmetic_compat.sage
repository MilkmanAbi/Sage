# RUN: bytecode-run
# EXPECT: 1679431879
# EXPECT: 0
# EXPECT: 0

var x = 39979

# Match the current AST/C backend modulo semantics, including the int cast.
print ((x * 1103515245) + 12345) % 2147483647

# Division and modulo by zero currently evaluate to 0.
println(7 % 0)
println(7 / 0)
