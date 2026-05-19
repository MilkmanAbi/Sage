# EXPECT: hello
# EXPECT: default
# EXPECT: 0
var a = "hello"
var b = None
print(a ?? "default")
print(b ?? "default")
print(None ?? 0)
