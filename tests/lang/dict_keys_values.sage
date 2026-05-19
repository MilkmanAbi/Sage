# EXPECT: 2
# EXPECT: 2
var d = {"x": 10, "y": 20}
var k = dict_keys(d)
var v = dict_values(d)
print(k.length)
print(v.length)
