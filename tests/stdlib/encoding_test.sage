gc_disable()
# EXPECT: SGVsbG8=
# EXPECT: Hello
# EXPECT: 48656c6c6f
# EXPECT: Hello
# EXPECT: SGVsbG8
# EXPECT: true

import crypto.encoding

# Base64 encode/decode
println(encoding.b64_encode("Hello"))
println(encoding.b64_decode_string("SGVsbG8="))

# Hex encode/decode
println(encoding.hex_encode("Hello"))
println(encoding.hex_decode_string("48656c6c6f"))

# URL-safe Base64
println(encoding.b64url_encode("Hello"))

# Round-trip
var original = [1, 2, 3, 4, 5]
var encoded = encoding.b64_encode(original)
var decoded = encoding.b64_decode(encoded)
var same = true
for i in 0..5:
    if original[i] != decoded[i]:
        same = false
println(same)
