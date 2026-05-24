gc_disable()
import net.server
# Parse a raw HTTP request
var raw = "GET /api/test?q=sage HTTP/1.1" + chr(13) + chr(10) + "Host: localhost" + chr(13) + chr(10) + "Accept: text/html" + chr(13) + chr(10) + chr(13) + chr(10)
var req = server.parse_request(raw)
println(req["method"])
println(req["path"])
println(req["query"])
println(req["headers"]["accept"])
# Test router
var router = server.create_router()
proc handler(r):
    return server.response_text("hello")
server.get_route(router, "GET", handler)
# Test response building
var resp = server.response_json("{}")
var has_json = false
for i in 0..resp.length:
    if i + 15 < resp.length:
        let sub = resp[i] + resp[i+1] + resp[i+2] + resp[i+3] + resp[i+4] + resp[i+5] + resp[i+6] + resp[i+7] + resp[i+8] + resp[i+9] + resp[i+10] + resp[i+11] + resp[i+12] + resp[i+13] + resp[i+14] + resp[i+15]
        if sub == "application/json":
            has_json = true
            i = resp.length
println(has_json)
