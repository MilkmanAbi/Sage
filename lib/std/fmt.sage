gc_disable()
# String formatting library
# printf-style and template-based string formatting

# ============================================================================
# Number formatting
# ============================================================================

proc pad_left(s, width, ch):
    var result = s
    while result.length < width:
        result = ch + result
    return result

proc pad_right(s, width, ch):
    var result = s
    while result.length < width:
        result = result + ch
    return result

# Format integer with comma separators: 1000000 -> "1,000,000"
@inline
proc format_int(n):
    var s = str(n)
    var neg = false
    if s.length > 0 and s[0] == "-":
        neg = true
        var tmp = ""
        for i in 0..s.length - 1:
            tmp = tmp + s[i + 1]
        s = tmp
    if s.length <= 3:
        if neg:
            return "-" + s
        return s
    var result = ""
    var count = 0
    var i = s.length - 1
    while i >= 0:
        if count > 0 and count - ((count / 3) | 0) * 3 == 0:
            result = "," + result
        result = s[i] + result
        count = count + 1
        i = i - 1
    if neg:
        return "-" + result
    return result

# Format float with fixed decimal places
@inline
proc format_float(n, decimals):
    let int_part = n | 0
    var frac = n - int_part
    if frac < 0:
        frac = 0 - frac
    var result = str(int_part) + "."
    for i in 0..decimals:
        frac = frac * 10
        let digit = (frac | 0) - (((frac | 0) / 10) | 0) * 10
        result = result + str(digit)
    return result

# Format as percentage
@inline
proc format_pct(n, decimals):
    return format_float(n * 100, decimals) + "%"

# Format bytes as human-readable
proc format_bytes(n):
    if n >= 1073741824:
        return format_float(n / 1073741824, 1) + " GB"
    if n >= 1048576:
        return format_float(n / 1048576, 1) + " MB"
    if n >= 1024:
        return format_float(n / 1024, 1) + " KB"
    return str(n) + " B"

# Format duration in seconds to human-readable
proc format_duration(secs):
    if secs < 0.001:
        return format_float(secs * 1000000, 0) + "us"
    if secs < 1:
        return format_float(secs * 1000, 1) + "ms"
    if secs < 60:
        return format_float(secs, 2) + "s"
    if secs < 3600:
        let mins = (secs / 60) | 0
        var s = secs - mins * 60
        return str(mins) + "m " + str(s | 0) + "s"
    let hrs = (secs / 3600) | 0
    let mins = ((secs - hrs * 3600) / 60) | 0
    return str(hrs) + "h " + str(mins) + "m"

# ============================================================================
# Hex formatting
# ============================================================================

@inline
proc to_hex(n, width):
    let digits = "0123456789abcdef"
    var result = ""
    var val = n
    if val == 0:
        result = "0"
    while val > 0:
        result = digits[val & 15] + result
        val = val >> 4
    return "0x" + pad_left(result, width, "0")

@inline
proc to_bin(n, width):
    var result = ""
    var val = n
    if val == 0:
        result = "0"
    while val > 0:
        if (val & 1) != 0:
            result = "1" + result
        else:
            result = "0" + result
        val = val >> 1
    return "0b" + pad_left(result, width, "0")

@inline
proc to_oct(n):
    var result = ""
    var val = n
    if val == 0:
        result = "0"
    while val > 0:
        result = str(val & 7) + result
        val = val >> 3
    return "0o" + result

# ============================================================================
# Template formatting
# ============================================================================

# Simple template: "Hello, {name}!" with dict {"name": "World"} -> "Hello, World!"
proc template(tmpl, values):
    var result = ""
    var i = 0
    while i < tmpl.length:
        if tmpl[i] == "{":
            var key = ""
            i = i + 1
            while i < tmpl.length and tmpl[i] != "}":
                key = key + tmpl[i]
                i = i + 1
            if i < tmpl.length:
                i = i + 1
            if dict_has(values, key):
                result = result + str(values[key])
            else:
                result = result + "{" + key + "}"
        else:
            result = result + tmpl[i]
            i = i + 1
    return result

# Join array elements with separator
proc join(arr, sep):
    var result = ""
    for i in 0..arr.length:
        if i > 0:
            result = result + sep
        result = result + str(arr[i])
    return result

# Repeat a character n times
proc repeat_char(ch, n):
    var result = ""
    for i in 0..n:
        result = result + ch
    return result

# Create a table-formatted string from headers and rows
proc table(headers, rows):
    let nl = chr(10)
    # Calculate column widths
    let widths = []
    for i in 0..headers.length:
        widths.push(str(headers[i]).length)
    for r in 0..rows.length:
        for c in 0..rows[r].length:
            if c < widths.length:
                let w = str(rows[r][c]).length
                if w > widths[c]:
                    widths[c] = w
    # Header row
    var result = ""
    for i in 0..headers.length:
        if i > 0:
            result = result + " | "
        result = result + pad_right(str(headers[i]), widths[i], " ")
    result = result + nl
    # Separator
    for i in 0..headers.length:
        if i > 0:
            result = result + "-+-"
        result = result + repeat_char("-", widths[i])
    result = result + nl
    # Data rows
    for r in 0..rows.length:
        for c in 0..rows[r].length:
            if c > 0:
                result = result + " | "
            if c < widths.length:
                result = result + pad_right(str(rows[r][c]), widths[c], " ")
        result = result + nl
    return result
