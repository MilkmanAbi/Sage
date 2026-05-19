# Conformance: Operator Precedence (Spec §4)
# EXPECT: 14
# EXPECT: 20
# EXPECT: true
# EXPECT: true
# EXPECT: 7
# * before +
println(2 + 3 * 4)
# Parentheses override
print (2 + 3) * 4
# Comparison and boolean
var x = 5
println(x > 3 and x < 10)
# Boolean operators
println(true or false)
# Bitwise operators
println(3 | 4)
