# EXPECT: olleh
# EXPECT: ababab
# EXPECT: true
import strings
println(strings.reverse("hello"))
println(strings.repeat("ab", 3))
println(strings.contains("hello world", "world"))
