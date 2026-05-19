import time

class Vec2:
    def __init__(self, x, y):
        self.x = x
        self.y = y
    def mag_sq(self):
        return self.x * self.x + self.y * self.y

start = time.time()
total = 0.0
for i in range(50000):
    v = Vec2(float(i), float(i + 1))
    total += v.mag_sq()
elapsed = time.time() - start
print(int(total))
print(f"time: {elapsed:.6f}s")
