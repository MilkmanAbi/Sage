# EXPECT: 8
# EXPECT: 4
# EXPECT: 42
# EXPECT: 20
# Test sizeof
println(sizeof(3.14))
println(sizeof("test"))

# Test unsafe block + pointer ops
unsafe:
    let p = mem_alloc(16)
    mem_write(p, 0, "int", 42)
    println(mem_read(p, 0, "int"))
    mem_free(p)

# Test ptr_add
var p2 = mem_alloc(64)
mem_write(p2, 0, "int", 10)
mem_write(p2, 4, "int", 20)
var p3 = ptr_add(p2, 4)
println(mem_read(p3, 0, "int"))
mem_free(p2)
