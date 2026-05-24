# Conformance: Truthiness (Spec §7 — updated v3.1.3)
# false, nil, 0, and "" are falsy; everything else is truthy.
# EXPECT: falsy
# EXPECT: falsy
# EXPECT: truthy
# EXPECT: truthy
# EXPECT: falsy
# EXPECT: falsy
# EXPECT: truthy
if 0:
    println("truthy")
else:
    println("falsy")
if "":
    println("truthy")
else:
    println("falsy")
if []:
    println("truthy")
else:
    println("falsy")
if 1:
    println("truthy")
else:
    println("falsy")
if false:
    println("truthy")
else:
    println("falsy")
if nil:
    println("truthy")
else:
    println("falsy")
if true:
    println("truthy")
else:
    println("falsy")
