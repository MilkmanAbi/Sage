gc_disable()
# EXPECT: 4
# EXPECT: 8
# EXPECT: .so
# EXPECT: true

import std.interop

println(interop.SIZEOF_INT)
println(interop.SIZEOF_POINTER)

println(interop.shared_lib_extension())

# Pack/unpack round-trip
var packed = interop.pack_i32(12345)
var unpacked = interop.unpack_i32(packed, 0)
println(unpacked == 12345)
