# Conformance: Coercion (Spec §7)
# Int + Float = Float, Int / Int = Float, % preserves float
# EXPECT: 5.5
# EXPECT: 2
# EXPECT: 0.7
# EXPECT: 3
# Mixed arithmetic
println(3 + 2.5)
# Division always returns float
println(5 / 2)
# Modulo preserves float
println(3.7 % 1.5)
# Integer ops return int
println(1 + 2)
