# Test inline assembly with arguments
# EXPECT: 42.0
# EXPECT: 42.0
# EXPECT: 100.0

# Add two numbers (System V ABI: rdi + rsi)
var sum = asm_exec("    mov %rdi, %rax\n    add %rsi, %rax", "int", 10, 32)
println(sum)

# Multiply two numbers
var product = asm_exec("    mov %rdi, %rax\n    imul %rsi, %rax", "int", 7, 6)
println(product)

# Three arguments: a + b + c
var sum3 = asm_exec("    mov %rdi, %rax\n    add %rsi, %rax\n    add %rdx, %rax", "int", 30, 30, 40)
println(sum3)
