# EXPECT: got 42
# EXPECT: nothing
# EXPECT: ok: hello
# EXPECT: err: oops
proc check(opt):
    match opt:
        Some(v) => return "got " + str(v)
        None    => return "nothing"
        _       => return "?"
print(check(Some(42)))
print(check(None))

proc show(r):
    match r:
        Ok(v)  => return "ok: " + str(v)
        Err(e) => return "err: " + e
        _      => return "?"
print(show(Ok("hello")))
print(show(Err("oops")))
