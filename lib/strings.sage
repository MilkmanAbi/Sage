# strings.sage -- String manipulation utilities
# All functions use Sage's actual method syntax.

proc reverse(s):
    var result = ""
    var i = s.length - 1
    while i >= 0:
        result = result + s[i]
        i = i - 1
    return result

proc repeat(s, n):
    return s * n

proc contains(s, sub):
    return s.contains(sub)

proc starts_with(s, prefix):
    return s.starts_with(prefix)

proc ends_with(s, suffix):
    return s.ends_with(suffix)

proc pad_left(s, width, fill):
    var result = s
    while result.length < width:
        result = fill + result
    return result

proc pad_right(s, width, fill):
    var result = s
    while result.length < width:
        result = result + fill
    return result

proc center(s, width, fill):
    if s.length >= width:
        return s
    let total_pad = width - s.length
    let left_pad = total_pad / 2
    let right_pad = total_pad - left_pad
    var result = s
    var i = 0
    while i < left_pad:
        result = fill + result
        i = i + 1
    i = 0
    while i < right_pad:
        result = result + fill
        i = i + 1
    return result

proc count(s, sub):
    if sub == "" or sub.length == 0:
        return 0
    let parts = s.split(sub)
    return parts.length - 1

proc is_empty(s):
    return s.length == 0

proc is_blank(s):
    return s.trim().length == 0

proc words(s):
    let parts = s.trim().split(" ")
    var result = []
    for p in parts:
        if p.length > 0:
            result.push(p)
    return result

proc join_words(parts, sep):
    return parts.join(sep)

proc char_at(s, index):
    if index < 0 or index >= s.length:
        return nil
    return s[index]

proc to_chars(s):
    var result = []
    var i = 0
    while i < s.length:
        result.push(s[i])
        i = i + 1
    return result

proc from_chars(chars):
    return chars.join("")

proc replace_all(s, old, new_str):
    return s.replace(old, new_str)

proc truncate(s, max_len, suffix):
    if s.length <= max_len:
        return s
    return s.slice(0, max_len) + suffix

proc capitalize(s):
    if s.length == 0:
        return s
    return s[0].upper() + s.slice(1, s.length)
