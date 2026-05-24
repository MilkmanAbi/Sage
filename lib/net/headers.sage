gc_disable()
# net/headers — HTTP header parsing and access

let TYPE_HTML = "text/html"
let TYPE_JSON = "application/json"
let TYPE_PNG  = "image/png"
let TYPE_CSS  = "text/css"
let TYPE_JS   = "application/javascript"
let TYPE_TEXT = "text/plain"
let TYPE_XML  = "application/xml"

proc _lower(s):
    var r = ""
    var i = 0
    while i < s.length:
        let c = s[i]
        let o = ord(c)
        if o >= 65 and o <= 90:
            r = r + chr(o + 32)
        else:
            r = r + c
        i = i + 1
    return r

proc _trim(s):
    var start = 0
    while start < s.length and (s[start] == " " or s[start] == chr(9)):
        start = start + 1
    var str_end = s.length - 1
    while str_end >= start and (s[str_end] == " " or s[str_end] == chr(9) or s[str_end] == chr(13) or s[str_end] == chr(10)):
        str_end = str_end - 1
    var result = ""
    var i = start
    while i <= str_end:
        result = result + s[i]
        i = i + 1
    return result

proc parse(raw):
    let h = {}
    let lines = raw.split(chr(10))
    for i in 0..lines.length:
        let line = _trim(lines[i])
        if line.length == 0:
            i = lines.length
        else:
            var colon = -1
            var j = 0
            while j < line.length:
                if line[j] == ":" and colon < 0:
                    colon = j
                j = j + 1
            if colon > 0:
                var k = ""
                var ki = 0
                while ki < colon:
                    k = k + line[ki]
                    ki = ki + 1
                var v = ""
                var vi = colon + 1
                while vi < line.length:
                    v = v + line[vi]
                    vi = vi + 1
                h[_lower(k.trim())] = v.trim()
    return h

proc get(h, key):
    let lk = _lower(key)
    if dict_has(h, lk):
        return h[lk]
    return nil

proc has(h, key):
    return dict_has(h, _lower(key))

proc set(h, key, value):
    h[_lower(key)] = value
    return h

proc content_type(h):
    let ct = get(h, "content-type")
    if ct == nil:
        return nil
    # Strip parameters like "; charset=utf-8"
    var i = 0
    while i < ct.length:
        if ct[i] == ";":
            var trimmed = ""
            var j = 0
            while j < i:
                trimmed = trimmed + ct[j]
                j = j + 1
            return trimmed.trim()
        i = i + 1
    return ct.trim()

proc is_html(h):
    let ct = content_type(h)
    return ct == "text/html"

proc is_json(h):
    let ct = content_type(h)
    return ct == "application/json"

proc is_image(h):
    let ct = content_type(h)
    if ct == nil:
        return false
    return ct.length >= 5 and ct[0] == "i" and ct[1] == "m"
