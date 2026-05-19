gc_disable()
# Symmetric cipher utilities
# XOR cipher, RC4 stream cipher, and block cipher mode helpers

proc u32(x):
    return x & 4294967295

proc str_to_bytes(s):
    let bytes = []
    for i in 0..s.length:
        bytes.push(ord(s[i]))
    return bytes

# ============================================================================
# XOR Cipher (repeating key)
# ============================================================================

proc xor_encrypt(data, key):
    var d = data
    if type(data) == "string":
        d = str_to_bytes(data)
    var k = key
    if type(key) == "string":
        k = str_to_bytes(key)
    let result = []
    for i in 0..d.length:
        result.push(d[i] ^ k[i & (k.length - 1)])
    return result

# XOR decrypt is identical to encrypt
proc xor_decrypt(data, key):
    return xor_encrypt(data, key)

# ============================================================================
# RC4 Stream Cipher
# ============================================================================

# Initialize RC4 key schedule (KSA)
proc rc4_init(key):
    var k = key
    if type(key) == "string":
        k = str_to_bytes(key)
    let s = []
    for i in 0..256:
        s.push(i)
    var j = 0
    for i in 0..256:
        j = (j + s[i] + k[i & (k.length - 1)]) & 255
        let temp = s[i]
        s[i] = s[j]
        s[j] = temp
    let state = {}
    state["s"] = s
    state["i"] = 0
    state["j"] = 0
    return state

# Generate next RC4 keystream byte
proc rc4_next(state):
    let s = state["s"]
    state["i"] = (state["i"] + 1) & 255
    var i = state["i"]
    state["j"] = (state["j"] + s[i]) & 255
    var j = state["j"]
    let temp = s[i]
    s[i] = s[j]
    s[j] = temp
    return s[(s[i] + s[j]) & 255]

# Encrypt/decrypt data using RC4
proc rc4(key, data):
    var d = data
    if type(data) == "string":
        d = str_to_bytes(data)
    let state = rc4_init(key)
    let result = []
    for i in 0..d.length:
        result.push(d[i] ^ rc4_next(state))
    return result

# ============================================================================
# Block Cipher Mode Helpers (for use with external block ciphers)
# ============================================================================

# PKCS#7 padding
proc pkcs7_pad(data, block_size):
    var d = data
    if type(data) == "string":
        d = str_to_bytes(data)
    var pad_len = block_size - (d.length & (block_size - 1))
    if pad_len == 0:
        pad_len = block_size
    let result = []
    for i in 0..d.length:
        result.push(d[i])
    for i in 0..pad_len:
        result.push(pad_len)
    return result

# Remove PKCS#7 padding
proc pkcs7_unpad(data):
    if data.length == 0:
        return data
    var pad_len = data[data.length - 1]
    if pad_len > data.length or pad_len == 0:
        return data
    # Verify all padding bytes
    var valid = true
    for i in 0..pad_len:
        if data[data.length - 1 - i] != pad_len:
            valid = false
    if not valid:
        return data
    let result = []
    for i in 0..data.length - pad_len:
        result.push(data[i])
    return result

# XOR two blocks of equal length
proc xor_blocks(a, b):
    let result = []
    for i in 0..a.length:
        result.push(a[i] ^ b[i])
    return result

# CBC mode encrypt (takes a block encrypt function, IV, and padded data)
# block_encrypt_fn: proc(block, key) -> encrypted block (byte arrays)
proc cbc_encrypt(block_encrypt_fn, key, iv, data):
    let block_size = iv.length
    let result = []
    var prev = iv
    var i = 0
    while i < data.length:
        let block = []
        for j in 0..block_size:
            if i + j < data.length:
                block.push(data[i + j])
            else:
                block.push(0)
        let xored = xor_blocks(block, prev)
        let encrypted = block_encrypt_fn(xored, key)
        for j in 0..encrypted.length:
            result.push(encrypted[j])
        prev = encrypted
        i = i + block_size
    return result

# CBC mode decrypt
proc cbc_decrypt(block_decrypt_fn, key, iv, data):
    let block_size = iv.length
    let result = []
    var prev = iv
    var i = 0
    while i < data.length:
        let block = []
        for j in 0..block_size:
            if i + j < data.length:
                block.push(data[i + j])
            else:
                block.push(0)
        let decrypted = block_decrypt_fn(block, key)
        let xored = xor_blocks(decrypted, prev)
        for j in 0..xored.length:
            result.push(xored[j])
        prev = block
        i = i + block_size
    return result

# CTR mode (encrypt and decrypt are identical)
proc ctr(block_encrypt_fn, key, nonce, data):
    let block_size = nonce.length
    let result = []
    var counter = 0
    var i = 0
    while i < data.length:
        # Build counter block: nonce + counter (big-endian in last 4 bytes)
        let ctr_block = []
        for j in 0..nonce.length:
            ctr_block.push(nonce[j])
        # Overwrite last 4 bytes with counter
        var ctr_off = ctr_block.length - 4
        if ctr_off < 0:
            ctr_off = 0
        ctr_block[ctr_off] = (counter >> 24) & 255
        ctr_block[ctr_off + 1] = (counter >> 16) & 255
        ctr_block[ctr_off + 2] = (counter >> 8) & 255
        ctr_block[ctr_off + 3] = counter & 255
        let keystream = block_encrypt_fn(ctr_block, key)
        for j in 0..block_size:
            if i + j < data.length:
                result.push(data[i + j] ^ keystream[j])
        counter = counter + 1
        i = i + block_size
    return result
