# EXPECT: other
# EXPECT: one
match 99:
    1 => print("one")
    _ => print("other")
match 1:
    1 => print("one")
    _ => print("other")
