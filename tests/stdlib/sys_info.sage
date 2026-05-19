# EXPECT: linux
# EXPECT: 0.2.0-alpha
# Test sys module info
import sys
println(sys.platform)
println(sys.version)
