gc_disable()
# EXPECT: true
# EXPECT: true
# EXPECT: false

import crypto.hmac
import crypto.hash

# HMAC-SHA256 with known key and message
var mac = hmac.hmac(hash.sha256, "key", "message", 64)
println(mac.length == 32)

# Constant-time compare: equal
var a = [1, 2, 3, 4]
var b = [1, 2, 3, 4]
println(hmac.secure_compare(a, b))

# Constant-time compare: not equal
var c = [1, 2, 3, 5]
println(hmac.secure_compare(a, c))
