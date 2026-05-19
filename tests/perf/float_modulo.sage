# EXPECT: 0.7
# EXPECT: 0.5
# EXPECT: 1
# EXPECT: 0
# Float modulo preserves float semantics
println(3.7 % 1.5)
println(2.5 % 1)
# Integer modulo still works
println(7 % 3)
println(10 % 5)
