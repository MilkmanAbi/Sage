# utils.sage -- General purpose utilities

proc identity(x):
    return x

proc constant(val):
    proc _fn():
        return val
    return _fn

proc compose(f, g):
    proc _composed(x):
        return f(g(x))
    return _composed

proc pipe(value, fns):
    var result = value
    for fn in fns:
        result = fn(result)
    return result

proc memoize(fn):
    var cache = {}
    proc _memo(arg):
        let key = str(arg)
        if cache.contains_key(key):
            return cache[key]
        let result = fn(arg)
        cache[key] = result
        return result
    return _memo

proc times(n, fn):
    var i = 0
    while i < n:
        fn(i)
        i = i + 1

proc clamp(val, low, high):
    if val < low:
        return low
    if val > high:
        return high
    return val

proc lerp(a, b, t):
    return a + (b - a) * t

proc swap(arr, i, j):
    let temp = arr[i]
    arr[i] = arr[j]
    arr[j] = temp
