gc_disable()
# HMAC (Hash-based Message Authentication Code)
# RFC 2104 implementation using pluggable hash functions

# Compute HMAC with a given hash function
# hash_fn: a proc that takes byte array and returns byte array (e.g., hash.sha256)
# key: byte array or string
# message: byte array or string
# block_size: hash block size in bytes (64 for SHA-256/SHA-1, 64 for MD5)
proc hmac(hash_fn, key, message, block_size):
    var k = key
    if type(key) == "string":
        k = str_to_bytes(key)
    var msg = message
    if type(message) == "string":
        msg = str_to_bytes(message)

    # If key is longer than block size, hash it
    if k.length > block_size:
        k = hash_fn(k)

    # Pad key to block_size with zeros
    let padded_key = []
    for i in 0..k.length:
        padded_key.push(k[i])
    while padded_key.length < block_size:
        padded_key.push(0)

    # Inner padding (key XOR 0x36)
    let i_key_pad = []
    for i in 0..block_size:
        i_key_pad.push(padded_key[i] ^ 54)

    # Outer padding (key XOR 0x5C)
    let o_key_pad = []
    for i in 0..block_size:
        o_key_pad.push(padded_key[i] ^ 92)

    # inner_hash = hash(i_key_pad + message)
    let inner_input = []
    for i in 0..i_key_pad.length:
        inner_input.push(i_key_pad[i])
    for i in 0..msg.length:
        inner_input.push(msg[i])
    let inner_hash = hash_fn(inner_input)

    # outer_hash = hash(o_key_pad + inner_hash)
    let outer_input = []
    for i in 0..o_key_pad.length:
        outer_input.push(o_key_pad[i])
    for i in 0..inner_hash.length:
        outer_input.push(inner_hash[i])

    return hash_fn(outer_input)

proc str_to_bytes(s):
    let bytes = []
    for i in 0..s.length:
        bytes.push(ord(s[i]))
    return bytes

proc to_hex(bytes):
    let digits = "0123456789abcdef"
    var result = ""
    for i in 0..bytes.length:
        result = result + digits[(bytes[i] >> 4) & 15] + digits[bytes[i] & 15]
    return result

# Convenience: HMAC-SHA256
proc hmac_sha256(key, message):
    # Import must happen at module level in Sage, so we take hash_fn as param
    # Users should call: hmac.hmac(hash.sha256, key, msg, 64)
    # This is a wrapper that requires hash module to be passed
    return nil

# Constant-time comparison of two byte arrays (prevents timing attacks)
proc secure_compare(a, b):
    if a.length != b.length:
        return false
    var result = 0
    for i in 0..a.length:
        result = result | (a[i] ^ b[i])
    return result == 0
