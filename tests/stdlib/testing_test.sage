gc_disable()
# EXPECT: true
# EXPECT: 2
# EXPECT: 0

import std.testing

var suite = testing.create_suite("Demo")

proc test_add():
    testing.assert_equal(1 + 1, 2, "basic add")

proc test_string():
    testing.assert_contains("hello world", "world", "contains")

testing.add_test(suite, "addition", test_add)
testing.add_test(suite, "string", test_string)

testing.run(suite)
println(suite["passed"] == 2)
println(suite["passed"])
println(suite["failed"])
