gc_disable()
# net/request — HTTP request/response helpers

proc create(method, url):
    let r = {}
    r["method"] = method
    r["url"] = url
    r["headers"] = {}
    r["body"] = nil
    return r

proc status_text(code):
    if code == 200:
        return "OK"
    if code == 201:
        return "Created"
    if code == 204:
        return "No Content"
    if code == 301:
        return "Moved Permanently"
    if code == 302:
        return "Found"
    if code == 304:
        return "Not Modified"
    if code == 400:
        return "Bad Request"
    if code == 401:
        return "Unauthorized"
    if code == 403:
        return "Forbidden"
    if code == 404:
        return "Not Found"
    if code == 405:
        return "Method Not Allowed"
    if code == 409:
        return "Conflict"
    if code == 422:
        return "Unprocessable Entity"
    if code == 429:
        return "Too Many Requests"
    if code == 500:
        return "Internal Server Error"
    if code == 502:
        return "Bad Gateway"
    if code == 503:
        return "Service Unavailable"
    return "Unknown"

proc is_ok(resp):
    return resp["status"] >= 200 and resp["status"] < 300

proc is_redirect(resp):
    return resp["status"] >= 300 and resp["status"] < 400

proc is_client_error(resp):
    return resp["status"] >= 400 and resp["status"] < 500

proc is_server_error(resp):
    return resp["status"] >= 500 and resp["status"] < 600

proc make_response(status, body, content_type):
    let r = {}
    r["status"] = status
    r["body"] = body
    r["headers"] = {}
    r["headers"]["content-type"] = content_type
    return r

proc get(url):
    import socket
    return socket.http_get(url)
