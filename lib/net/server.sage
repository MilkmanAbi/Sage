gc_disable()
# net/server — HTTP server utilities (request parsing, response building, routing)

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

proc s.trim():
    var start = 0
    while start < s.length and (s[start] == " " or s[start] == chr(9) or s[start] == chr(13)):
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

proc parse_request(raw):
    let lines = raw.split(chr(10))
    let req = {}
    req["method"] = ""
    req["path"] = "/"
    req["query"] = ""
    req["version"] = "HTTP/1.1"
    req["headers"] = {}
    req["body"] = ""
    # First line: METHOD PATH?QUERY VERSION
    if lines.length > 0:
        let parts = lines[0].trim().split(" ")
        if parts.length >= 1:
            req["method"] = parts[0]
        if parts.length >= 2:
            let p = parts[1]
            var q_idx = -1
            var i = 0
            while i < p.length:
                if p[i] == "?":
                    q_idx = i
                i = i + 1
            if q_idx >= 0:
                var path = ""
                var j = 0
                while j < q_idx:
                    path = path + p[j]
                    j = j + 1
                req["path"] = path
                var query = ""
                var k = q_idx + 1
                while k < p.length:
                    query = query + p[k]
                    k = k + 1
                req["query"] = query
            else:
                req["path"] = p
        if parts.length >= 3:
            req["version"] = parts[2]
    # Headers (lines 1 until blank)
    var i = 1
    var in_body = false
    while i < lines.length:
        let line = _trim(lines[i])
        if line.length == 0:
            in_body = true
        elif in_body:
            if req["body"].length > 0:
                req["body"] = req["body"] + chr(10)
            req["body"] = req["body"] + lines[i]
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
                req["headers"][_lower(k.trim())] = v.trim()
        i = i + 1
    return req

proc response_text(body):
    return "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: " + str(body.length) + "\r\n\r\n" + body

proc response_html(body):
    return "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: " + str(body.length) + "\r\n\r\n" + body

proc response_json(body):
    return "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + str(body.length) + "\r\n\r\n" + body

proc response_status(code, body):
    return "HTTP/1.1 " + str(code) + " OK\r\nContent-Length: " + str(body.length) + "\r\n\r\n" + body

proc create_router():
    var r = {}
    r["routes"] = []
    return r

proc get_route(router, method, handler):
    let route = {}
    route["method"] = method
    route["pattern"] = "/"
    route["handler"] = handler
    router["routes"].push(route)

proc add_route(router, method, pattern, handler):
    let route = {}
    route["method"] = method
    route["pattern"] = pattern
    route["handler"] = handler
    router["routes"].push(route)
