gc_disable()
# EXPECT: 64
# EXPECT: 40
# EXPECT: 32
# EXPECT: 20
# EXPECT: true
# EXPECT: 8

import crypto.hash

# SHA-256 produces 64-char hex string
var h256 = hash.sha256_hex("hello")
println(h256.length)

# SHA-1 produces 40-char hex string
var h1 = hash.sha1_hex("hello")
println(h1.length)

# SHA-256 produces 32 bytes
var h256b = hash.sha256("hello")
println(h256b.length)

# SHA-1 produces 20 bytes
var h1b = hash.sha1("hello")
println(h1b.length)

# Different inputs produce different hashes
var ha = hash.sha256_hex("abc")
var hb = hash.sha256_hex("def")
println(ha != hb)

# CRC-32 produces a hex string
var c = hash.crc32_hex("hello")
println(c.length)
