import time
start = time.time()
result = ""
for i in range(10000):
    result += str(i)
length = len(result)
elapsed = time.time() - start
print(length)
print(f"time: {elapsed:.6f}s")
