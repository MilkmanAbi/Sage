# EXPECT: 3
# EXPECT: nil
# Generic type annotations (parsed but not enforced yet)
let nums: Array[Int] = [10, 20, 30]
println(nums.length)

let lookup: Dict[String, Int] = {"a": 1, "b": 2}
println(lookup.length)
