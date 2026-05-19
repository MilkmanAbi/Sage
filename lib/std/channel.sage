gc_disable()
# Go-style channels for message passing between threads
# Provides buffered and unbuffered channels with send/recv/select

# ============================================================================
# Channel creation
# ============================================================================

proc create(capacity):
    let ch = {}
    ch["buffer"] = []
    ch["capacity"] = capacity
    ch["closed"] = false
    ch["send_count"] = 0
    ch["recv_count"] = 0
    return ch

# Unbuffered channel (synchronous)
@inline
proc unbuffered():
    return create(0)

# Buffered channel
@inline
proc buffered(size):
    return create(size)

# ============================================================================
# Send / Receive
# ============================================================================

# Send a value to the channel
proc send(ch, value):
    if ch["closed"]:
        raise "send on closed channel"
    if ch["capacity"] > 0 and ch["buffer"].length >= ch["capacity"]:
        raise "channel buffer full"
    ch["buffer"].push(value)
    ch["send_count"] = ch["send_count"] + 1
    return true

# Try to send (non-blocking, returns bool)
proc try_send(ch, value):
    if ch["closed"]:
        return false
    if ch["capacity"] > 0 and ch["buffer"].length >= ch["capacity"]:
        return false
    ch["buffer"].push(value)
    ch["send_count"] = ch["send_count"] + 1
    return true

# Receive a value from the channel
proc recv(ch):
    if ch["buffer"].length == 0:
        if ch["closed"]:
            return nil
        return nil
    let val = ch["buffer"][0]
    # Shift buffer left
    let new_buf = []
    for i in 0..ch["buffer"].length - 1:
        new_buf.push(ch["buffer"][i + 1])
    ch["buffer"] = new_buf
    ch["recv_count"] = ch["recv_count"] + 1
    return val

# Try to receive (non-blocking)
proc try_recv(ch):
    if ch["buffer"].length == 0:
        let result = {}
        result["ok"] = false
        result["value"] = nil
        return result
    let val = recv(ch)
    let result = {}
    result["ok"] = true
    result["value"] = val
    return result

# Close a channel
@inline
proc close(ch):
    ch["closed"] = true

# Check if channel is empty
@inline
proc is_empty(ch):
    return ch["buffer"].length == 0

# Check if channel is full
proc is_full(ch):
    if ch["capacity"] == 0:
        return ch["buffer"].length > 0
    return ch["buffer"].length >= ch["capacity"]

# Number of items in buffer
@inline
proc pending(ch):
    return ch["buffer"].length

# Check if channel is closed
@inline
proc is_closed(ch):
    return ch["closed"]

# ============================================================================
# Select (poll multiple channels)
# ============================================================================

# Poll multiple channels, return index of first ready channel + value
proc select(channels):
    for i in 0..channels.length:
        if channels[i]["buffer"].length > 0:
            let result = {}
            result["index"] = i
            result["value"] = recv(channels[i])
            return result
    return nil

# ============================================================================
# Fan-out / Fan-in patterns
# ============================================================================

# Send values from array to channel
proc send_all(ch, values):
    for i in 0..values.length:
        send(ch, values[i])

# Drain all values from channel into array
proc drain(ch):
    let result = []
    while ch["buffer"].length > 0:
        result.push(recv(ch))
    return result

# Pipe: forward all from source to dest
proc pipe(source, dest):
    while source["buffer"].length > 0:
        send(dest, recv(source))

# ============================================================================
# Channel stats
# ============================================================================

proc stats(ch):
    let s = {}
    s["capacity"] = ch["capacity"]
    s["pending"] = ch["buffer"].length
    s["sent"] = ch["send_count"]
    s["received"] = ch["recv_count"]
    s["closed"] = ch["closed"]
    return s
