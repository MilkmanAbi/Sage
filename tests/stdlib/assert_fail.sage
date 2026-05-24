# EXPECT: caught
import assert
try:
    assert.equal(1, 2)
catch e:
    println("caught")
