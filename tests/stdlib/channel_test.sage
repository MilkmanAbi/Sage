gc_disable()
# EXPECT: true
# EXPECT: 42
# EXPECT: 1
# EXPECT: true
# EXPECT: true

import std.channel

var ch = channel.buffered(10)
channel.send(ch, 42)
channel.send(ch, 99)
println(channel.pending(ch) > 0)
println(channel.recv(ch))
println(channel.pending(ch))

# Drain
var vals = channel.drain(ch)
println(vals.length == 1)

# Select
var ch2 = channel.buffered(5)
var result = channel.select([ch, ch2])
println(result == nil)
