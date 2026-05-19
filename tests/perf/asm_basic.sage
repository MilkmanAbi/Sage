# Test basic inline assembly - return a constant
# EXPECT: 42.0

var result = asm_exec("    mov $42, %rax", "int")
println(result)
