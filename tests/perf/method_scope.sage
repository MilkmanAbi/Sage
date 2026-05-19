# EXPECT: 1.4.0
# EXPECT: 100
# EXPECT: 1.4.0
# Test class methods can see module-level let bindings
var VERSION = "1.4.0"
var MAX_SIZE = 100

class Config:
    proc init(self):
        self.ver = VERSION
        self.max = MAX_SIZE

    proc show_version(self):
        println(VERSION)

    proc show_max(self):
        println(self.max)

var c = Config()
c.show_version()
c.show_max()
println(c.ver)
