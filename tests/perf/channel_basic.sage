# EXPECT: 0
# EXPECT: 1
# EXPECT: 2
# EXPECT: nil
import channel
var ch = channel.new()
async proc fill(ch):
    var i = 0
    while i < 3:
        channel.send(ch, i)
        i += 1
    channel.close(ch)
await fill(ch)
print(channel.recv(ch))
print(channel.recv(ch))
print(channel.recv(ch))
print(channel.recv(ch))
