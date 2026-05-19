gc_disable()
# EXPECT: https
# EXPECT: example.com
# EXPECT: 443
# EXPECT: /path/to
# EXPECT: key=val
# EXPECT: frag
# EXPECT: hello%20world
# EXPECT: hello world
# EXPECT: val
# EXPECT: https://example.com/path/to?key=val#frag

import net.url

var u = url.parse("https://example.com/path/to?key=val#frag")
println(u["scheme"])
println(u["host"])
println(u["port"])
println(u["path"])
println(u["query"])
println(u["fragment"])

println(url.encode("hello world"))
println(url.decode("hello%20world"))

var params = url.parse_query("key=val&name=test")
println(params["key"])

println(url.build(u))
