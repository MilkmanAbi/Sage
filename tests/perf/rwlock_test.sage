# EXPECT: read_lock_ok
# EXPECT: write_lock_ok
# EXPECT: try_lock_ok
# EXPECT: scoped_read_ok
# EXPECT: scoped_write_ok
# EXPECT: stats_ok
# EXPECT: PASS
from std.rwlock import create, read_lock, read_unlock, write_lock, write_unlock
from std.rwlock import try_read_lock, try_write_lock, is_read_locked, is_write_locked
from std.rwlock import reader_count, stats, with_read, with_write

# --- multiple readers ---
var rw = create()
read_lock(rw)
read_lock(rw)
if reader_count(rw) == 2 and is_read_locked(rw):
    read_unlock(rw)
    read_unlock(rw)
    if reader_count(rw) == 0 and is_read_locked(rw) == false:
        println("read_lock_ok")

# --- exclusive writer ---
var rw2 = create()
write_lock(rw2)
if is_write_locked(rw2):
    write_unlock(rw2)
    if is_write_locked(rw2) == false:
        println("write_lock_ok")

# --- try_lock ---
var rw3 = create()
var r1 = try_read_lock(rw3)
var r2 = try_read_lock(rw3)
if r1 == true and r2 == true:
    # try_write_lock fails while readers hold
    let w1 = try_write_lock(rw3)
    if w1 == false:
        read_unlock(rw3)
        read_unlock(rw3)
        let w2 = try_write_lock(rw3)
        if w2 == true:
            # try_read_lock fails while writer holds
            let r3 = try_read_lock(rw3)
            if r3 == false:
                write_unlock(rw3)
                println("try_lock_ok")

# --- with_read scoped helper ---
var rw4 = create()
var shared_data = 42
proc read_fn():
    return shared_data
var result = with_read(rw4, read_fn)
if result == 42 and is_read_locked(rw4) == false:
    println("scoped_read_ok")

# --- with_write scoped helper ---
var rw5 = create()
var counter = 0
proc write_fn():
    counter = counter + 10
    return counter
var wresult = with_write(rw5, write_fn)
if wresult == 10 and is_write_locked(rw5) == false:
    println("scoped_write_ok")

# --- stats ---
var rw6 = create()
read_lock(rw6)
read_lock(rw6)
read_unlock(rw6)
read_unlock(rw6)
write_lock(rw6)
write_unlock(rw6)
var st = stats(rw6)
if st["read_ops"] == 2 and st["write_ops"] == 1 and st["readers"] == 0 and st["writer"] == false:
    println("stats_ok")

println("PASS")
