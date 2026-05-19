# EXPECT: Some(42)
# EXPECT: nil
# EXPECT: 42
# EXPECT: fallback
# EXPECT: Some(99)
print(Some(42))
print(None)
print(Some(42)!)
var x = None ?? "fallback"
print(x)
var y = Some(99)
var z = y ?? "empty"
print(z)
