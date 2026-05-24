# RUN: emit-c
# EXPECT_ERROR: error: unknown name 'coutn' in compiled code
# EXPECT_ERROR: compiler_unknown_name.sage:6:7
# EXPECT_ERROR: help: did you mean 'count'?
var count = 1
println(coutn)
