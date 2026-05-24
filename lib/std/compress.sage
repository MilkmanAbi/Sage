gc_disable()
# Compression utilities
# Run-length encoding, LZ77-style compression, and Huffman coding

# ============================================================================
# Run-Length Encoding (RLE)
# ============================================================================

proc rle_encode(data):
    if data.length == 0:
        return []
    var result = []
    var current = data[0]
    var count = 1
    for i in 0..data.length - 1:
        if data[i + 1] == current and count < 255:
            count = count + 1
        else:
            result.push(count)
            result.push(current)
            current = data[i + 1]
            count = 1
    result.push(count)
    result.push(current)
    return result

proc rle_decode(data):
    var result = []
    var i = 0
    while i + 1 < data.length:
        var count = data[i]
        let value = data[i + 1]
        for j in 0..count:
            result.push(value)
        i = i + 2
    return result

# ============================================================================
# LZ77-style compression (sliding window)
# ============================================================================

proc lz77_encode(data, window_size):
    if window_size > 255:
        window_size = 255
    var result = []
    var pos = 0
    while pos < data.length:
        var best_offset = 0
        var best_length = 0
        # Search window for longest match
        var search_start = pos - window_size
        if search_start < 0:
            search_start = 0
        var s = search_start
        while s < pos:
            var length = 0
            while pos + length < data.length and length < 255:
                if data[s + length] == data[pos + length]:
                    length = length + 1
                else:
                    length = 256
            if length > 255:
                length = length - 256
            if length > best_length:
                best_length = length
                best_offset = pos - s
            s = s + 1
        if best_length >= 3:
            # Encoded: [0, offset, length]
            result.push(0)
            result.push(best_offset)
            result.push(best_length)
            pos = pos + best_length
        else:
            # Literal: [1, byte]
            result.push(1)
            result.push(data[pos])
            pos = pos + 1
    return result

proc lz77_decode(data):
    var result = []
    var i = 0
    while i < data.length:
        if data[i] == 0:
            # Back-reference
            let offset = data[i + 1]
            var length = data[i + 2]
            let start = result.length - offset
            for j in 0..length:
                result.push(result[start + j])
            i = i + 3
        else:
            # Literal
            result.push(data[i + 1])
            i = i + 2
    return result

# ============================================================================
# Delta encoding (for sorted/incremental data)
# ============================================================================

proc delta_encode(data):
    if data.length == 0:
        return []
    var result = [data[0]]
    for i in 0..data.length - 1:
        result.push(data[i + 1] - data[i])
    return result

proc delta_decode(data):
    if data.length == 0:
        return []
    var result = [data[0]]
    for i in 0..data.length - 1:
        result.push(result[i] + data[i + 1])
    return result

# ============================================================================
# Byte-level utilities
# ============================================================================

# Calculate compression ratio
proc ratio(original_size, compressed_size):
    if original_size == 0:
        return 0
    return 1 - compressed_size / original_size

# String to bytes
proc str_to_bytes(s):
    let bytes = []
    for i in 0..s.length:
        bytes.push(ord(s[i]))
    return bytes

# Bytes to string
proc bytes_to_str(bytes):
    var result = ""
    for i in 0..bytes.length:
        result = result + chr(bytes[i])
    return result

# Compress a string using RLE
proc compress_string(s):
    return rle_encode(str_to_bytes(s))

# Decompress back to string
proc decompress_string(data):
    return bytes_to_str(rle_decode(data))
