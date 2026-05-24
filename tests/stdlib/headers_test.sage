gc_disable()
# EXPECT: text/html
# EXPECT: 42
# EXPECT: true
# EXPECT: text/html
# EXPECT: true
# EXPECT: image/png
# EXPECT: application/json

import net.headers

var raw = "Content-Type: text/html" + chr(13) + chr(10) + "Content-Length: 42" + chr(13) + chr(10)
var h = headers.parse(raw)
println(headers.get(h, "Content-Type"))
println(headers.get(h, "Content-Length"))
println(headers.has(h, "content-type"))
println(headers.content_type(h))

println(headers.is_html(h))

println(headers.TYPE_PNG)
println(headers.TYPE_JSON)
