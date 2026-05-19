# EXPECT: 2
# EXPECT: -1
# EXPECT: [5, 4, 3, 2, 1]
# EXPECT: [3, 2]
# EXPECT: hello
# EXPECT: default
var a = [1, 2, 3, 4, 5]
println(a.indexOf(3))
println(a.indexOf(99))
a.reverse()
println(a)
println(a.slice(2, 4))

var d = {"key": "hello"}
println(d.get("key", "?"))
println(d.get("missing", "default"))
