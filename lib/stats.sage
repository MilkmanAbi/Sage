# stats.sage -- Basic statistics

proc sum(arr):
    var total = 0
    for x in arr:
        total = total + x
    return total

proc mean(arr):
    if arr.length == 0:
        return 0
    return float(sum(arr)) / float(arr.length)

proc median(arr):
    if arr.length == 0:
        return 0
    # Sort (insertion sort)
    var sorted = []
    for x in arr:
        sorted.push(x)
    var i = 1
    while i < sorted.length:
        let key = sorted[i]
        var j = i - 1
        while j >= 0 and sorted[j] > key:
            sorted[j + 1] = sorted[j]
            j = j - 1
        sorted[j + 1] = key
        i = i + 1
    let n = sorted.length
    if n % 2 == 1:
        return sorted[n / 2]
    return (float(sorted[n / 2 - 1]) + float(sorted[n / 2])) / 2.0

proc min_val(arr):
    if arr.length == 0:
        return nil
    var m = arr[0]
    for x in arr:
        if x < m:
            m = x
    return m

proc max_val(arr):
    if arr.length == 0:
        return nil
    var m = arr[0]
    for x in arr:
        if x > m:
            m = x
    return m

proc variance(arr):
    if arr.length == 0:
        return 0
    let m = mean(arr)
    var total = 0.0
    for x in arr:
        let d = float(x) - m
        total = total + d * d
    return total / float(arr.length)

proc stdev(arr):
    import math
    return math.sqrt(variance(arr))

proc count(arr, fn):
    var n = 0
    for x in arr:
        if fn(x):
            n = n + 1
    return n
