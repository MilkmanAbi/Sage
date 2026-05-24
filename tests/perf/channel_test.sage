# EXPECT: buffered_ok
# EXPECT: unbuffered_ok
# EXPECT: closed_ok
# EXPECT: try_send_ok
# EXPECT: try_recv_ok
# EXPECT: select_ok
# EXPECT: drain_ok
# EXPECT: pipe_ok
# EXPECT: stats_ok
# EXPECT: PASS
from std.channel import create, buffered, unbuffered, send, recv, try_send, try_recv
from std.channel import close, is_closed, is_empty, is_full, pending
from std.channel import select, send_all, drain, pipe, stats

# --- Buffered channel ---
var ch = buffered(3)
send(ch, 10)
send(ch, 20)
send(ch, 30)
if pending(ch) == 3:
    if is_full(ch):
        let v1 = recv(ch)
        let v2 = recv(ch)
        let v3 = recv(ch)
        if v1 == 10 and v2 == 20 and v3 == 30:
            if is_empty(ch):
                println("buffered_ok")

# --- Unbuffered channel (capacity 0 — acts as rendezvous) ---
var uch = unbuffered()
if uch["capacity"] == 0:
    println("unbuffered_ok")

# --- Close ---
var cch = buffered(2)
send(cch, 1)
close(cch)
if is_closed(cch):
    # recv on closed+non-empty still works
    let cv = recv(cch)
    if cv == 1:
        # recv on closed+empty returns nil
        let cv2 = recv(cch)
        if cv2 == nil:
            println("closed_ok")

# --- try_send: full channel returns false ---
var fch = buffered(1)
var ok1 = try_send(fch, 99)
var ok2 = try_send(fch, 100)  # full
if ok1 == true and ok2 == false:
    # try_send on closed returns false
    let cch2 = buffered(2)
    close(cch2)
    let ok3 = try_send(cch2, 1)
    if ok3 == false:
        println("try_send_ok")

# --- try_recv ---
var rch = buffered(2)
send(rch, 42)
var r1 = try_recv(rch)
var r2 = try_recv(rch)  # empty
if r1["ok"] == true and r1["value"] == 42:
    if r2["ok"] == false and r2["value"] == nil:
        println("try_recv_ok")

# --- select ---
var s1 = buffered(2)
var s2 = buffered(2)
send(s2, "hello")
var sel = select([s1, s2])
if sel["index"] == 1 and sel["value"] == "hello":
    println("select_ok")

# --- drain ---
var dch = buffered(5)
send_all(dch, [1, 2, 3])
var drained = drain(dch)
if drained.length == 3 and drained[0] == 1 and drained[2] == 3:
    if is_empty(dch):
        println("drain_ok")

# --- pipe ---
var src = buffered(3)
var dst = buffered(3)
send_all(src, [10, 20, 30])
pipe(src, dst)
if is_empty(src) and pending(dst) == 3:
    println("pipe_ok")

# --- stats ---
var sch = buffered(4)
send(sch, 1)
send(sch, 2)
recv(sch)
var st = stats(sch)
if st["sent"] == 2 and st["received"] == 1 and st["pending"] == 1 and st["capacity"] == 4:
    println("stats_ok")

println("PASS")
