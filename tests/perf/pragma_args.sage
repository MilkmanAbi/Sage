# EXPECT: aligned struct created
# EXPECT: 100
# Test pragma with arguments

@align("16")
struct AlignedData:
    value: Int

var d = AlignedData(100)
println("aligned struct created")
println(d.value)
