import time
start = time.time()
total = 0
for i in range(100000):
    arr = [i, i + 1, i + 2, i + 3, i + 4]
    total += arr[2]
elapsed = time.time() - start
print(total)
print(f"time: {elapsed:.6f}s")
