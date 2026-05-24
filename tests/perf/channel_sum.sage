# EXPECT: 10
import channel
var ch = channel.new()
async proc producer(ch):
    var i = 0
    while i < 5:
        channel.send(ch, i)
        i += 1
    channel.close(ch)
async proc consumer(ch):
    var total = 0
    var v = channel.recv(ch)
    while v != nil:
        total += v
        v = channel.recv(ch)
    return total
var p = producer(ch)
var c = consumer(ch)
await p
print(await c)
