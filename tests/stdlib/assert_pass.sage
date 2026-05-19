# EXPECT: ok
import assert
assert.equal(2, 2)
assert.is_true(true)
assert.is_false(false)
assert.is_nil(nil)
assert.is_type(42, "int")
println("ok")
