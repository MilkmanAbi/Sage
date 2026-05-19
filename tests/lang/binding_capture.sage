# EXPECT: negative: -3
# EXPECT: zero
# EXPECT: small: 7
# EXPECT: big: 42
proc label(n):
    match n:
        n if n < 0  => return "negative: " + str(n)
        0           => return "zero"
        n if n < 10 => return "small: " + str(n)
        n           => return "big: " + str(n)
print(label(-3))
print(label(0))
print(label(7))
print(label(42))
