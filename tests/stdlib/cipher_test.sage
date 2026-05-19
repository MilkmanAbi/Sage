gc_disable()
# EXPECT: true
# EXPECT: true
# EXPECT: 16
# EXPECT: 3

import crypto.cipher

# XOR encrypt/decrypt round-trip
var data = [72, 101, 108, 108, 111]
var key = [42, 17, 99]
var encrypted = cipher.xor_encrypt(data, key)
var decrypted = cipher.xor_decrypt(encrypted, key)
var same = true
for i in 0..5:
    if data[i] != decrypted[i]:
        same = false
println(same)

# RC4 encrypt/decrypt round-trip
var rc4_key = "secret"
var plaintext = [1, 2, 3, 4, 5, 6, 7, 8]
var ct = cipher.rc4(rc4_key, plaintext)
var pt = cipher.rc4(rc4_key, ct)
var rc4_same = true
for i in 0..8:
    if plaintext[i] != pt[i]:
        rc4_same = false
println(rc4_same)

# PKCS7 padding
var padded = cipher.pkcs7_pad([1, 2, 3], 16)
println(padded.length)

var unpadded = cipher.pkcs7_unpad(padded)
println(unpadded.length)
