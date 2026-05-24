# Test ffi_sym for checking symbol existence
# EXPECT: true
# EXPECT: false

var math = ffi_open("libm.so.6")
println(ffi_sym(math, "sqrt"))
println(ffi_sym(math, "nonexistent_function"))
ffi_close(math)
