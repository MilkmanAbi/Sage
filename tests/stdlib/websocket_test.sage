gc_disable()
# EXPECT: text
# EXPECT: 5
# EXPECT: true
# EXPECT: true
# EXPECT: hello
# EXPECT: close

import net.websocket

# Build a text frame
var frame = websocket.text_frame("hello")
println(websocket.opcode_name(1))
println(len("hello"))

# Parse the frame back
var parsed = websocket.parse_frame(frame, 0)
println(parsed["fin"])
println(parsed["opcode"] == 1)
println(websocket.payload_to_string(parsed["payload"]))

# Close frame
var cf = websocket.close_frame(1000)
var parsed_close = websocket.parse_frame(cf, 0)
println(parsed_close["opcode_name"])
