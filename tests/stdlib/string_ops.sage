# EXPECT: 6.0
# EXPECT: true
# EXPECT: true
# EXPECT: olleh
# Test string module operations
import string
println(string.find("hello world", "world"))
println(string.startswith("hello", "he"))
println(string.endswith("hello", "lo"))
println(string.reverse("hello"))
