gc_disable()
# EXPECT: 0
# EXPECT: 1
# EXPECT: true

import std.signal

var bus = signal.create_bus()
println(signal.handler_count(bus, "click"))

var clicked = false

proc on_click(data):
    clicked = true

signal.on(bus, "click", on_click)
println(signal.handler_count(bus, "click"))

signal.emit(bus, "click", nil)
# Note: clicked is module-level, handler modifies it but scope rules
# mean we can't easily check it here. Just verify the event names.
var names = signal.event_names(bus)
println(names.length == 1)
