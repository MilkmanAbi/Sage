# assert.sage -- Test assertions

proc equal(a, b):
    if a != b:
        raise "Assertion failed: " + str(a) + " != " + str(b)

proc not_equal(a, b):
    if a == b:
        raise "Assertion failed: " + str(a) + " == " + str(b) + " (expected different)"

proc is_true(val):
    if not val:
        raise "Assertion failed: expected true, got " + str(val)

proc is_false(val):
    if val:
        raise "Assertion failed: expected false, got " + str(val)

proc is_nil(val):
    if val != nil:
        raise "Assertion failed: expected nil, got " + str(val)

proc is_not_nil(val):
    if val == nil:
        raise "Assertion failed: expected non-nil value"

proc is_type(val, type_name):
    if typeof(val) != type_name:
        raise "Assertion failed: expected type " + type_name + ", got " + typeof(val)

proc contains(collection, item):
    if typeof(collection) == "str":
        if not collection.contains(str(item)):
            raise "Assertion failed: '" + str(collection) + "' does not contain '" + str(item) + "'"
    elif typeof(collection) == "Array":
        if not collection.contains(item):
            raise "Assertion failed: array does not contain " + str(item)
    else:
        raise "Assertion failed: contains() needs str or Array"

proc throws(fn):
    try:
        fn()
        raise "Assertion failed: expected exception but none was thrown"
    catch e:
        return e

proc near(a, b, epsilon):
    import math
    if math.abs(a - b) > epsilon:
        raise "Assertion failed: " + str(a) + " not near " + str(b) + " (epsilon=" + str(epsilon) + ")"
