gc_disable()
# EXPECT: true
# EXPECT: greet
# EXPECT: proc

import std.docgen

var source = "# Say hello" + chr(10) + "proc greet(name):" + chr(10) + "    print name" + chr(10)
var docs = docgen.extract_docs(source)
println(docs.length > 0)
println(docs[0]["name"])
println(docs[0]["type"])
