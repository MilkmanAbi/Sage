gc_disable()
# net/websocket — WebSocket frame encoding/decoding

proc opcode_name(op):
    if op == 0:
        return "continuation"
    if op == 1:
        return "text"
    if op == 2:
        return "binary"
    if op == 8:
        return "close"
    if op == 9:
        return "ping"
    if op == 10:
        return "pong"
    return "unknown"

# Build a WebSocket frame (server-to-client, no masking)
proc _build_frame(opcode, payload_bytes):
    let frame = []
    # Byte 1: FIN=1, opcode
    frame.push(128 + opcode)  # 0x80 | opcode
    var plen = payload_bytes.length
    if plen < 126:
        frame.push(plen)
    elif plen < 65536:
        frame.push(126)
        frame.push((plen / 256) | 0)
        frame.push(plen % 256)
    else:
        frame.push(127)
        # 8-byte length (simplified: only handle up to 32-bit)
        var i = 7
        while i >= 0:
            if i >= 4:
                frame.push(0)
            else:
                frame.push((plen >> (i * 8)) & 255)
            i = i - 1
    for i in 0..payload_bytes.length:
        frame.push(payload_bytes[i])
    return frame

proc _str_to_bytes(s):
    let b = []
    var i = 0
    while i < s.length:
        b.push(ord(s[i]))
        i = i + 1
    return b

proc text_frame(text):
    return _build_frame(1, _str_to_bytes(text))

proc binary_frame(data):
    return _build_frame(2, data)

proc close_frame(code):
    let payload = [(code / 256) | 0, code % 256]
    return _build_frame(8, payload)

proc ping_frame(data):
    return _build_frame(9, data)

proc pong_frame(data):
    return _build_frame(10, data)

proc parse_frame(data, offset):
    var result = {}
    if data.length < offset + 2:
        result["error"] = "too short"
        return result
    let b0 = data[offset]
    let b1 = data[offset + 1]
    result["fin"] = (b0 & 128) != 0
    result["opcode"] = b0 & 15
    result["opcode_name"] = opcode_name(result["opcode"])
    let masked = (b1 & 128) != 0
    result["masked"] = masked
    var plen = b1 & 127
    var header_len = 2
    if plen == 126:
        plen = data[offset + 2] * 256 + data[offset + 3]
        header_len = 4
    elif plen == 127:
        # Simplified: skip 8-byte length, just use header_len=10
        header_len = 10
        plen = 0
    if masked:
        header_len = header_len + 4
    # Extract payload
    let payload = []
    var i = offset + header_len
    while i < offset + header_len + plen and i < data.length:
        payload.push(data[i])
        i = i + 1
    # Unmask if needed
    if masked and data.length >= offset + header_len:
        let mask_start = offset + header_len - 4
        var j = 0
        while j < payload.length:
            payload[j] = payload[j] ^ data[mask_start + (j % 4)]
            j = j + 1
    result["payload"] = payload
    result["length"] = plen
    return result

proc payload_to_string(payload):
    var result = ""
    var i = 0
    while i < payload.length:
        result = result + chr(payload[i])
        i = i + 1
    return result
