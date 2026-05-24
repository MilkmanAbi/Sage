gc_disable()
# EXPECT: true
# EXPECT: true
# EXPECT: true

import std.compress

# RLE round-trip
var data = [1, 1, 1, 2, 2, 3]
var encoded = compress.rle_encode(data)
var decoded = compress.rle_decode(encoded)
var same = true
for i in 0..data.length:
    if data[i] != decoded[i]:
        same = false
println(same)

# Delta round-trip
var sorted_data = [10, 12, 15, 20, 28]
var delta_enc = compress.delta_encode(sorted_data)
var delta_dec = compress.delta_decode(delta_enc)
var delta_same = true
for i in 0..sorted_data.length:
    if sorted_data[i] != delta_dec[i]:
        delta_same = false
println(delta_same)

# LZ77 round-trip
var text = compress.str_to_bytes("abcabcabcabc")
var lz_enc = compress.lz77_encode(text, 32)
var lz_dec = compress.lz77_decode(lz_enc)
var lz_same = true
for i in 0..text.length:
    if text[i] != lz_dec[i]:
        lz_same = false
println(lz_same)
