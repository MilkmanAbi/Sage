gc_disable()
# net/url — URL parsing, encoding, building

proc encode(s):
    var result = ""
    var i = 0
    while i < s.length:
        let c = s[i]
        let o = ord(c)
        if (o >= 65 and o <= 90) or (o >= 97 and o <= 122) or (o >= 48 and o <= 57) or c == "-" or c == "_" or c == "." or c == "~":
            result = result + c
        else:
            let hex_chars = "0123456789ABCDEF"
            result = result + "%" + hex_chars[(o / 16) | 0] + hex_chars[o % 16]
        i = i + 1
    return result

proc decode(s):
    var result = ""
    var i = 0
    while i < s.length:
        if s[i] == "%" and i + 2 < s.length:
            let h1 = s[i+1]
            let h2 = s[i+2]
            var v1 = 0
            var v2 = 0
            if h1 >= "0" and h1 <= "9":
                v1 = ord(h1) - 48
            elif h1 >= "A" and h1 <= "F":
                v1 = ord(h1) - 55
            elif h1 >= "a" and h1 <= "f":
                v1 = ord(h1) - 87
            if h2 >= "0" and h2 <= "9":
                v2 = ord(h2) - 48
            elif h2 >= "A" and h2 <= "F":
                v2 = ord(h2) - 55
            elif h2 >= "a" and h2 <= "f":
                v2 = ord(h2) - 87
            result = result + chr(v1 * 16 + v2)
            i = i + 3
        elif s[i] == "+":
            result = result + " "
            i = i + 1
        else:
            result = result + s[i]
            i = i + 1
    return result

proc parse_query(qs):
    var result = {}
    if qs.length == 0:
        return result
    let pairs = qs.split("&")
    for i in 0..pairs.length:
        let pair = pairs[i]
        var eq = -1
        var j = 0
        while j < pair.length:
            if pair[j] == "=":
                eq = j
                j = pair.length
            j = j + 1
        if eq < 0:
            result[decode(pair)] = ""
        else:
            var k = ""
            var jj = 0
            while jj < eq:
                k = k + pair[jj]
                jj = jj + 1
            var v = ""
            var kk = eq + 1
            while kk < pair.length:
                v = v + pair[kk]
                kk = kk + 1
            result[decode(k)] = decode(v)
    return result

proc parse(u):
    var result = {}
    result["scheme"] = ""
    result["host"] = ""
    result["port"] = 0
    result["path"] = "/"
    result["query"] = ""
    result["fragment"] = ""
    # Find scheme
    var schemestr_end = 0
    var i = 0
    while i < u.length:
        if u[i] == ":":
            schemestr_end = i
            i = u.length
        i = i + 1
    var scheme = ""
    var j = 0
    while j < schemestr_end:
        scheme = scheme + u[j]
        j = j + 1
    result["scheme"] = scheme
    var default_port = 80
    if scheme == "https":
        default_port = 443
    elif scheme == "ftp":
        default_port = 21
    # Skip ://
    var pos = schemestr_end + 3
    # Find host str_end (/ ? # or str_end)
    var hoststr_end = pos
    while hoststr_end < u.length and u[hoststr_end] != "/" and u[hoststr_end] != "?" and u[hoststr_end] != "#":
        hoststr_end = hoststr_end + 1
    # Check for port in host
    var host_part = ""
    var ii = pos
    while ii < hoststr_end:
        host_part = host_part + u[ii]
        ii = ii + 1
    # Split host:port
    var colon = -1
    var ci = 0
    while ci < host_part.length:
        if host_part[ci] == ":":
            colon = ci
        ci = ci + 1
    if colon >= 0:
        var h = ""
        var hi = 0
        while hi < colon:
            h = h + host_part[hi]
            hi = hi + 1
        result["host"] = h
        var port_str = ""
        var pi = colon + 1
        while pi < host_part.length:
            port_str = port_str + host_part[pi]
            pi = pi + 1
        result["port"] = int(port_str)
    else:
        result["host"] = host_part
        result["port"] = default_port
    pos = hoststr_end
    # Path
    if pos < u.length and u[pos] != "?" and u[pos] != "#":
        var path = ""
        while pos < u.length and u[pos] != "?" and u[pos] != "#":
            path = path + u[pos]
            pos = pos + 1
        result["path"] = path
    # Query
    if pos < u.length and u[pos] == "?":
        pos = pos + 1
        var query = ""
        while pos < u.length and u[pos] != "#":
            query = query + u[pos]
            pos = pos + 1
        result["query"] = query
    # Fragment
    if pos < u.length and u[pos] == "#":
        pos = pos + 1
        var frag = ""
        while pos < u.length:
            frag = frag + u[pos]
            pos = pos + 1
        result["fragment"] = frag
    return result

proc build(u):
    var result = u["scheme"] + "://" + u["host"]
    var scheme = u["scheme"]
    let port = u["port"]
    if scheme == "http" and port != 80:
        result = result + ":" + str(port)
    elif scheme == "https" and port != 443:
        result = result + ":" + str(port)
    result = result + u["path"]
    if u["query"].length > 0:
        result = result + "?" + u["query"]
    if u["fragment"].length > 0:
        result = result + "#" + u["fragment"]
    return result
