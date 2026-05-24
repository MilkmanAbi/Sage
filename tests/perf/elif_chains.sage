# EXPECT: seven
# EXPECT: other
# EXPECT: one
# Test long elif chain (8 branches - previously broken at 5+)
var x = 7
if x == 1:
    println("one")
elif x == 2:
    println("two")
elif x == 3:
    println("three")
elif x == 4:
    println("four")
elif x == 5:
    println("five")
elif x == 6:
    println("six")
elif x == 7:
    println("seven")
elif x == 8:
    println("eight")
else:
    println("other")

# Test fallthrough to else
var y = 99
if y == 1:
    println("one")
elif y == 2:
    println("two")
elif y == 3:
    println("three")
elif y == 4:
    println("four")
elif y == 5:
    println("five")
else:
    println("other")

# Test first branch
var z = 1
if z == 1:
    println("one")
elif z == 2:
    println("two")
elif z == 3:
    println("three")
