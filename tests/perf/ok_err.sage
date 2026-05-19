# EXPECT: Ok(1)
# EXPECT: Err(bad)
# EXPECT: 1
# EXPECT: Err(bad)
print(Ok(1))
print(Err("bad"))
print(Ok(1)!)
print(Err("bad"))
