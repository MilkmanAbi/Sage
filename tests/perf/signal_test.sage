# EXPECT: on_ok
# EXPECT: once_ok
# EXPECT: off_ok
# EXPECT: multi_handler_ok
# EXPECT: atexit_ok
# EXPECT: PASS
from std.signal import create_bus, on, once, emit, off, clear, handler_count, event_names
from std.signal import atexit, run_atexit

# --- on / emit ---
var bus = create_bus()
var received = 0
proc handler1(data):
    received = received + data
on(bus, "tick", handler1)
emit(bus, "tick", 5)
emit(bus, "tick", 3)
if received == 8:
    println("on_ok")

# --- once: fires only once ---
var bus2 = create_bus()
var once_count = 0
proc once_handler(data):
    once_count = once_count + 1
once(bus2, "start", once_handler)
emit(bus2, "start", nil)
emit(bus2, "start", nil)  # should not fire again
if once_count == 1:
    println("once_ok")

# --- off: removes handlers ---
var bus3 = create_bus()
var fired = 0
proc h(data):
    fired = fired + 1
on(bus3, "ev", h)
emit(bus3, "ev", nil)
off(bus3, "ev")
emit(bus3, "ev", nil)  # should not fire
if fired == 1:
    if handler_count(bus3, "ev") == 0:
        println("off_ok")

# --- multiple handlers on same event ---
var bus4 = create_bus()
var total = 0
proc ha(data):
    total = total + 1
proc hb(data):
    total = total + 10
proc hc(data):
    total = total + 100
on(bus4, "multi", ha)
on(bus4, "multi", hb)
on(bus4, "multi", hc)
emit(bus4, "multi", nil)
if total == 111:
    let names = event_names(bus4)
    if names.length == 1 and names[0] == "multi":
        println("multi_handler_ok")

# --- atexit (LIFO order) ---
var order = []
proc exit1():
    order.push(1)
proc exit2():
    order.push(2)
proc exit3():
    order.push(3)
atexit(exit1)
atexit(exit2)
atexit(exit3)
run_atexit()
# LIFO: 3, 2, 1
if order.length == 3 and order[0] == 3 and order[1] == 2 and order[2] == 1:
    println("atexit_ok")

println("PASS")
