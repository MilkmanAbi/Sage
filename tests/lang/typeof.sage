# EXPECT: int
# EXPECT: float
# EXPECT: str
# EXPECT: bool
# EXPECT: nil
# EXPECT: Array
# EXPECT: Dict
println(typeof(42))
println(typeof(3.14))
println(typeof("hello"))
println(typeof(true))
println(typeof(nil))
println(typeof([1,2,3]))
println(typeof({"a": 1}))
