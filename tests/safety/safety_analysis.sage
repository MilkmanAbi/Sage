gc_disable()
# EXPECT: analysis_ok
# EXPECT: PASS

# This file is tested with: sage safety tests/28_safety/safety_analysis.sage
# It should pass safety analysis with no errors.

# Good ownership patterns
var x = 42
var y = x

# Safe function with annotated parameters
proc add(a, b):
    return a + b

var result = add(10, 20)

# Safe control flow
if result == 30:
    let inner = "hello"

# Safe loop
var total = 0
var i = 0
while i < 5:
    total = total + i
    i = i + 1

# Safe class
proc make_point(px, py):
    let p = {}
    p["x"] = px
    p["y"] = py
    return p

var p = make_point(3, 4)

if p["x"] == 3:
    if p["y"] == 4:
        println("analysis_ok")

println("PASS")
