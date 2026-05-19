# dicts.sage -- Dictionary utilities

proc merge(a, b):
    var result = {}
    for k in a.keys():
        result[k] = a[k]
    for k in b.keys():
        result[k] = b[k]
    return result

proc get_or(d, key, default_val):
    let v = d.get(key)
    if v == nil:
        return default_val
    return v

proc has(d, key):
    return d.contains_key(key)

proc map_values(d, fn):
    var result = {}
    for k in d.keys():
        result[k] = fn(d[k])
    return result

proc filter_keys(d, fn):
    var result = {}
    for k in d.keys():
        if fn(k):
            result[k] = d[k]
    return result

proc invert(d):
    var result = {}
    for k in d.keys():
        result[str(d[k])] = k
    return result

proc from_pairs(pairs):
    var result = {}
    for pair in pairs:
        result[pair[0]] = pair[1]
    return result

proc to_pairs(d):
    var result = []
    for k in d.keys():
        result.push([k, d[k]])
    return result

proc pick(d, keys_list):
    var result = {}
    for k in keys_list:
        if d.contains_key(k):
            result[k] = d[k]
    return result

proc omit(d, keys_list):
    var result = {}
    let skip = {}
    for k in keys_list:
        skip[k] = true
    for k in d.keys():
        if not skip.contains_key(k):
            result[k] = d[k]
    return result
