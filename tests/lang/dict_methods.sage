# EXPECT: Sage
# EXPECT: true
# EXPECT: false
var d = {"name": "Abi", "lang": "Sage"}
println(d.lang)
println(d.contains_key("name"))
println(d.contains_key("nope"))
