# EXPECT: 5
# EXPECT: 104
# EXPECT: ABC
# EXPECT: el
# Test Bytes type
var b = bytes("hello")
println(bytes_len(b))
println(bytes_get(b, 0))

var b2 = bytes([65, 66, 67])
println(bytes_to_string(b2))

var b3 = bytes_slice(b, 1, 3)
println(bytes_to_string(b3))
