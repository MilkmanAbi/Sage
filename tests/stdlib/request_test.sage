gc_disable()
# EXPECT: GET
# EXPECT: https://example.com
# EXPECT: OK
# EXPECT: Not Found
# EXPECT: true
# EXPECT: true
# EXPECT: false

import net.request

var req = request.create("GET", "https://example.com")
println(req["method"])
println(req["url"])

println(request.status_text(200))
println(request.status_text(404))

# Test response classification helpers with mock response
var mock_ok = {}
mock_ok["status"] = 200
println(request.is_ok(mock_ok))

var mock_err = {}
mock_err["status"] = 500
println(request.is_server_error(mock_err))
println(request.is_ok(mock_err))
