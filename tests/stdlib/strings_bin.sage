# EXPECT: --------hi
# EXPECT: hi--------
import strings
println(strings.pad_left("hi", 10, "-"))
println(strings.pad_right("hi", 10, "-"))
