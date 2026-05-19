# RUN: bytecode-run
# EXPECT: 9
# EXPECT: 7

proc add(a, b):
    return a + b

class Counter:
    proc init(self, start):
        self.value = start

    proc bump(self, delta):
        self.value = self.value + delta
        return self.value

println(add(4, 5))

var counter = Counter(2)
println(counter.bump(5))
