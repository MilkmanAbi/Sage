# bench_struct.sage — Struct creation + method dispatch
struct Vec2:
    x: float
    y: float

impl Vec2:
    proc mag_sq(self):
        return self.x * self.x + self.y * self.y

let start = clock()
var total = 0.0
var i = 0
while i < 50000:
    let v = Vec2(float(i), float(i + 1))
    total = total + v.mag_sq()
    i = i + 1
let elapsed = clock() - start
println(int(total))
println("time: " + str(elapsed) + "s")
