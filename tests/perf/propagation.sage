# EXPECT: Some(5)
# EXPECT: nil
proc safe_div(a, b):
    if b == 0:
        return None
    return Some(a / b)

proc compute(a, b):
    let r = safe_div(a, b)?
    return Some(r)

print(compute(10, 2))
print(compute(10, 0) ?? None)
