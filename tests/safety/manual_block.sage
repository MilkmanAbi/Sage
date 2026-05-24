# EXPECT: 1337
# EXPECT: 999
# EXPECT: done

@manual:
    var ptr = mem_alloc(32)
    mem_write(ptr, 0, "int", 1337)
    println(mem_read(ptr, 0, "int"))
    mem_free(ptr)

gc_disable()
var ptr2 = mem_alloc(16)
mem_write(ptr2, 0, "int", 999)
println(mem_read(ptr2, 0, "int"))
mem_free(ptr2)
gc_enable()

println("done")
