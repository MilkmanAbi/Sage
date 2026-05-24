# EXPECT: [1, 2, 3, 4]
# EXPECT: 4
# EXPECT: 4
# EXPECT: [1, 2, 3]
# EXPECT: true
# EXPECT: false
# EXPECT: 1-2-3
var a = [1, 2, 3]
a.push(4)
println(a)
println(a.length)
println(a.pop())
println(a)
println(a.contains(2))
println(a.contains(99))
println(a.join("-"))
