# EXPECT: 4
# EXPECT: hello from spawn
import channel
var ch = channel.new()
spawn:
    channel.send(ch, 4)
print(channel.recv(ch))
var msg_ch = channel.new()
spawn:
    channel.send(msg_ch, "hello from spawn")
print(channel.recv(msg_ch))
