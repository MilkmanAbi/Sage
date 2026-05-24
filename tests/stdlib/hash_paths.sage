# EXPECT: true
# EXPECT: true
# EXPECT: usr/local/bin
# EXPECT: /home/user
# EXPECT: file.sage
# EXPECT: .sage
# Test hash
println(hash("hello") == hash("hello"))
println(hash("hello") != hash("world"))

# Test path utilities
println(path_join("usr", "local", "bin"))
println(path_dirname("/home/user/file.sage"))
println(path_basename("/home/user/file.sage"))
println(path_ext("/home/user/file.sage"))
