# Conformance: Exceptions (Spec §9)
# EXPECT: caught: bad
# EXPECT: cleanup
# EXPECT: instance error: oops
# String exceptions
try:
    raise "bad"
catch e:
    println("caught: " + e)
finally:
    println("cleanup")

# Instance exceptions
class AppError:
    proc init(self, msg):
        self.msg = msg

try:
    raise AppError("oops")
catch e:
    println("instance error: " + e.msg)
