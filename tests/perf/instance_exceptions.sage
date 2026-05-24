# EXPECT: caught: divide by zero
# EXPECT: done
# Test raising class instances as exceptions
class AppError:
    proc init(self, msg):
        self.msg = msg

try:
    raise AppError("divide by zero")
catch e:
    println("caught: " + e.msg)
finally:
    println("done")
