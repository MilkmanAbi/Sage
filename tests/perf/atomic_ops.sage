# EXPECT: 200.0
# EXPECT: true
# EXPECT: 10.0
# EXPECT: false
import atomic
var ctr = atomic.new(0)
async proc bump(ctr, n):
    var i = 0
    while i < n:
        atomic.add(ctr, 1)
        i += 1
await bump(ctr, 100)
await bump(ctr, 100)
print(atomic.load(ctr))
var a = atomic.new(5)
print(atomic.cas(a, 5, 10))
print(atomic.load(a))
print(atomic.cas(a, 5, 99))
