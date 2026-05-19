# iter.sage -- Iteration utilities

proc range_list(start, stop):
    var result = []
    var i = start
    while i < stop:
        result.push(i)
        i = i + 1
    return result

proc range_step(start, stop, step):
    var result = []
    var i = start
    if step > 0:
        while i < stop:
            result.push(i)
            i = i + step
    elif step < 0:
        while i > stop:
            result.push(i)
            i = i + step
    return result

proc repeat_val(value, n):
    var result = []
    var i = 0
    while i < n:
        result.push(value)
        i = i + 1
    return result

proc chain(a, b):
    var result = []
    for item in a:
        result.push(item)
    for item in b:
        result.push(item)
    return result

proc take_while(arr, fn):
    var result = []
    for item in arr:
        if not fn(item):
            break
        result.push(item)
    return result

proc drop_while(arr, fn):
    var result = []
    var dropping = true
    for item in arr:
        if dropping and fn(item):
            continue
        dropping = false
        result.push(item)
    return result
