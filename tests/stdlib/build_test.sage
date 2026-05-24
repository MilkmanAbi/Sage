gc_disable()
# EXPECT: myapp
# EXPECT: 1.0.0
# EXPECT: 1
# EXPECT: 2.0.0.0
# EXPECT: true

import std.build

var proj = build.create_project("myapp", "1.0.0")
println(proj["name"])
println(proj["version"])

build.add_dep(proj, "json", ">=1.0")
println(proj["dependencies"].length)

# Version parsing
var v = build.parse_version("1.5.3")
var v2 = build.bump_major(v)
println(v2["string"])

# Serialization
var output = build.to_string(proj)
println(output.length > 0)
