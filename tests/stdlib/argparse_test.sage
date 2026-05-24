gc_disable()
# EXPECT: true
# EXPECT: out.txt
# EXPECT: 1
# EXPECT: true

import std.argparse

var parser = argparse.create("myapp", "A test app")
argparse.add_flag(parser, "verbose", "v", "Enable verbose output")
argparse.add_option(parser, "output", "o", "Output file", "default.txt")

var result = argparse.parse(parser, ["--verbose", "-o", "out.txt", "input.sage"])
println(argparse.get_flag(result, "verbose"))
println(argparse.get_option(result, "output"))
println(result["positionals"].length)

# Help text
var help = argparse.help_text(parser)
println(help.length > 0)
