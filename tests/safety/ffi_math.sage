# Test FFI with libm math functions
# EXPECT: 4.0
# EXPECT: 1024.0
# EXPECT: 5.0
# EXPECT: 4.0

var math = ffi_open("libm.so.6")
println(ffi_call(math, "sqrt", "double", [16.0]))
println(ffi_call(math, "pow", "double", [2.0, 10.0]))
println(ffi_call(math, "ceil", "double", [4.3]))
println(ffi_call(math, "floor", "double", [4.7]))
ffi_close(math)
