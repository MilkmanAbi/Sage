# EXPECT: 42
# EXPECT: default2
# EXPECT: error: bad
proc try_parse(s):
    if s == "42":
        return Ok(42)
    return Err("bad")

match try_parse("42"):
    Ok(v)  => print(v)
    Err(e) => print("error: " + e)

# For Result, use match to get default, not ??
proc unwrap_or(r, fallback):
    match r:
        Ok(v)  => return v
        Err(e) => return fallback
        _      => return fallback
print(unwrap_or(try_parse("nope"), "default2"))

match try_parse("nope"):
    Ok(v)  => print(v)
    Err(e) => print("error: " + e)
