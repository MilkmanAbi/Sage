# arrays.sage -- Array manipulation utilities

proc map(arr, fn):
    var result = []
    for item in arr:
        result.push(fn(item))
    return result

proc filter(arr, fn):
    var result = []
    for item in arr:
        if fn(item):
            result.push(item)
    return result

proc reduce(arr, fn, start_val):
    var acc = start_val
    for item in arr:
        acc = fn(acc, item)
    return acc

proc each(arr, fn):
    for item in arr:
        fn(item)

proc find(arr, fn):
    for item in arr:
        if fn(item):
            return item
    return nil

proc find_index(arr, fn):
    var i = 0
    while i < arr.length:
        if fn(arr[i]):
            return i
        i = i + 1
    return -1

proc every(arr, fn):
    for item in arr:
        if not fn(item):
            return false
    return true

proc some(arr, fn):
    for item in arr:
        if fn(item):
            return true
    return false

proc sort(arr):
    # Simple insertion sort -- works for now
    let n = arr.length
    var result = []
    for item in arr:
        result.push(item)
    var i = 1
    while i < n:
        let key = result[i]
        var j = i - 1
        while j >= 0 and result[j] > key:
            result[j + 1] = result[j]
            j = j - 1
        result[j + 1] = key
        i = i + 1
    return result

proc flatten(arr):
    var result = []
    for item in arr:
        if typeof(item) == "Array":
            let inner = flatten(item)
            for sub in inner:
                result.push(sub)
        else:
            result.push(item)
    return result

proc zip(a, b):
    var result = []
    var i = 0
    let len = a.length
    if b.length < len:
        let len = b.length
    while i < len:
        result.push([a[i], b[i]])
        i = i + 1
    return result

proc enumerate(arr):
    var result = []
    var i = 0
    while i < arr.length:
        result.push([i, arr[i]])
        i = i + 1
    return result

proc unique(arr):
    var seen = {}
    var result = []
    for item in arr:
        let key = str(item)
        if not seen.contains_key(key):
            seen[key] = true
            result.push(item)
    return result

proc chunk(arr, size):
    var result = []
    var i = 0
    while i < arr.length:
        var group = []
        var j = 0
        while j < size and (i + j) < arr.length:
            group.push(arr[i + j])
            j = j + 1
        result.push(group)
        i = i + size
    return result

proc sum(arr):
    var total = 0
    for item in arr:
        total = total + item
    return total

proc min_val(arr):
    if arr.length == 0:
        return nil
    var m = arr[0]
    var i = 1
    while i < arr.length:
        if arr[i] < m:
            m = arr[i]
        i = i + 1
    return m

proc max_val(arr):
    if arr.length == 0:
        return nil
    var m = arr[0]
    var i = 1
    while i < arr.length:
        if arr[i] > m:
            m = arr[i]
        i = i + 1
    return m

proc take(arr, n):
    var result = []
    var i = 0
    while i < n and i < arr.length:
        result.push(arr[i])
        i = i + 1
    return result

proc drop(arr, n):
    var result = []
    var i = n
    while i < arr.length:
        result.push(arr[i])
        i = i + 1
    return result

proc count(arr, fn):
    var n = 0
    for item in arr:
        if fn(item):
            n = n + 1
    return n
