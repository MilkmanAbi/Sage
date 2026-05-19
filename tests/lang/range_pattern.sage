# EXPECT: small
# EXPECT: medium
# EXPECT: large
proc classify(n):
    match n:
        0..10   => return "small"
        10..100 => return "medium"
        _       => return "large"
print(classify(5))
print(classify(50))
print(classify(999))
