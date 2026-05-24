# EXPECT: Some(10)
# EXPECT: true
import channel
var q = channel.new()
channel.send(q, 10)
channel.send(q, 20)
print(channel.q.length)
print(channel.try_recv(q))
channel.close(q)
print(channel.is_closed(q))
