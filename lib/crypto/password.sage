gc_disable()
# Password hashing and verification utilities
# PBKDF2-HMAC-SHA256 key derivation

# PBKDF2 key derivation function
# Uses HMAC with a provided hash function
# password: string or byte array
# salt: string or byte array
# iterations: number of HMAC rounds (recommended >= 10000)
# key_length: desired output length in bytes
# hash_fn: hash function (e.g., hash.sha256)
# block_size: hash block size (64 for SHA-256)
proc pbkdf2(hash_fn, password, salt, iterations, key_length, block_size):
    var pwd = password
    if type(password) == "string":
        pwd = str_to_bytes(password)
    var s = salt
    if type(salt) == "string":
        s = str_to_bytes(salt)

    var result = []
    var block_num = 1
    let hash_len = len(hash_fn(pwd))

    while result.length < key_length:
        # U1 = HMAC(password, salt + INT32_BE(block_num))
        let salt_block = []
        for i in 0..s.length:
            salt_block.push(s[i])
        salt_block.push((block_num >> 24) & 255)
        salt_block.push((block_num >> 16) & 255)
        salt_block.push((block_num >> 8) & 255)
        salt_block.push(block_num & 255)

        var u = hmac_raw(hash_fn, pwd, salt_block, block_size)
        let dk = []
        for i in 0..u.length:
            dk.push(u[i])

        # U2..Uc
        for iter in 0..iterations - 1:
            u = hmac_raw(hash_fn, pwd, u, block_size)
            for i in 0..dk.length:
                dk[i] = dk[i] ^ u[i]

        for i in 0..dk.length:
            if result.length < key_length:
                result.push(dk[i])
        block_num = block_num + 1

    return result

# Internal HMAC (duplicated here to avoid circular import)
proc hmac_raw(hash_fn, key, message, block_size):
    var k = key
    if k.length > block_size:
        k = hash_fn(k)
    let padded_key = []
    for i in 0..k.length:
        padded_key.push(k[i])
    while padded_key.length < block_size:
        padded_key.push(0)
    let i_key_pad = []
    for i in 0..block_size:
        i_key_pad.push(padded_key[i] ^ 54)
    let o_key_pad = []
    for i in 0..block_size:
        o_key_pad.push(padded_key[i] ^ 92)
    let inner_input = []
    for i in 0..i_key_pad.length:
        inner_input.push(i_key_pad[i])
    for i in 0..message.length:
        inner_input.push(message[i])
    let inner_hash = hash_fn(inner_input)
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

# Constant-time comparison
proc secure_compare(a, b):
    if a.length != b.length:
        return false
    var result = 0
    for i in 0..a.length:
        result = result | (a[i] ^ b[i])
    return result == 0

# Generate a password hash string: "pbkdf2:iterations:salt_hex:hash_hex"
proc hash_password(hash_fn, password, salt_bytes, iterations, block_size):
    let key = pbkdf2(hash_fn, password, salt_bytes, iterations, 32, block_size)
    let salt_hex = to_hex(salt_bytes)
    let key_hex = to_hex(key)
    return "pbkdf2:" + str(iterations) + ":" + salt_hex + ":" + key_hex

# Verify a password against a hash string
proc verify_password(hash_fn, password, hash_string, block_size):
    # Parse "pbkdf2:iterations:salt_hex:hash_hex"
    let parts = split_colon(hash_string)
    if parts.length != 4:
        return false
    let iterations = tonumber(parts[1])
    let salt_bytes = hex_decode(parts[2])
    let expected = hex_decode(parts[3])
    let derived = pbkdf2(hash_fn, password, salt_bytes, iterations, expected.length, block_size)
    return secure_compare(derived, expected)

proc split_colon(s):
    let parts = []
    var current = ""
    for i in 0..s.length:
        if s[i] == ":":
            parts.push(current)
            current = ""
        else:
            current = current + s[i]
    if current.length > 0:
        parts.push(current)
    return parts

proc hex_decode(encoded):
    var result = []
    var i = 0
    while i + 1 < encoded.length:
        let hi = hex_val(encoded[i])
        let lo = hex_val(encoded[i + 1])
        if hi >= 0 and lo >= 0:
            result.push(hi * 16 + lo)
        i = i + 2
    return result

proc hex_val(c):
    let code = ord(c)
    if code >= 48 and code <= 57:
        return code - 48
    if code >= 65 and code <= 70:
        return code - 55
    if code >= 97 and code <= 102:
        return code - 87
    return -1
