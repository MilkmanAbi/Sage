# Test FFI with libc functions
# EXPECT: 42
# EXPECT: 5

var libc = ffi_open("libc.so.6")
println(ffi_call(libc, "abs", "int", [-42]))
println(ffi_call(libc, "strlen", "long", ["hello"]))
ffi_close(libc)
