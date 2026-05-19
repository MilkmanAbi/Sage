gc_disable()
# EXPECT: some_created
# EXPECT: none_created
# EXPECT: unwrap_ok
# EXPECT: unwrap_or_ok
# EXPECT: map_ok
# EXPECT: and_then_ok
# EXPECT: filter_ok
# EXPECT: option_str_ok
# EXPECT: PASS

import safety

# Test Some creation
var x = safety.Some(42)
if safety.is_some(x):
    if safety.is_none(x) == false:
        println("some_created")

# Test None creation
var y = safety.None()
if safety.is_none(y):
    if safety.is_some(y) == false:
        println("none_created")

# Test unwrap
var val = safety.unwrap(x)
if val == 42:
    println("unwrap_ok")

# Test unwrap_or
var val2 = safety.unwrap_or(y, 99)
if val2 == 99:
    let val3 = safety.unwrap_or(x, 99)
    if val3 == 42:
        println("unwrap_or_ok")

# Test map
proc double(n):
    return n * 2

var mapped = safety.map(x, double)
var mapped_val = safety.unwrap(mapped)
if mapped_val == 84:
    let mapped_none = safety.map(y, double)
    if safety.is_none(mapped_none):
        println("map_ok")

# Test and_then
proc safe_div(n):
    if n == 0:
        return safety.None()
    return safety.Some(100 / n)

var result = safety.and_then(safety.Some(5), safe_div)
if safety.unwrap(result) == 20:
    let result2 = safety.and_then(safety.None(), safe_div)
    if safety.is_none(result2):
        println("and_then_ok")

# Test filter
proc is_positive(n):
    return n > 0

var filtered = safety.filter(safety.Some(10), is_positive)
if safety.is_some(filtered):
    let filtered2 = safety.filter(safety.Some(-5), is_positive)
    if safety.is_none(filtered2):
        println("filter_ok")

# Test option_to_str
var s1 = safety.option_to_str(safety.Some(42))
var s2 = safety.option_to_str(safety.None())
if contains(s1, "Some"):
    if s2 == "None":
        println("option_str_ok")

println("PASS")
