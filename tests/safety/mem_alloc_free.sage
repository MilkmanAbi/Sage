# Test mem_alloc and mem_free
# EXPECT: 64
# EXPECT: done

var ptr = mem_alloc(64)
println(mem_size(ptr))
mem_free(ptr)
println("done")
