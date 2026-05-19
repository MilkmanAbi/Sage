gc_disable()
# EXPECT: 32
# EXPECT: true

import crypto.hash
import crypto.password

# PBKDF2 produces correct length output
var key = password.pbkdf2(hash.sha256, "password", "salt", 1, 32, 64)
println(key.length)

# Constant-time compare works
var a = [1, 2, 3]
var b = [1, 2, 3]
println(password.secure_compare(a, b))
