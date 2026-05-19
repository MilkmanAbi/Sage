gc_disable()
# EXPECT: 2024-03-15T10:30:00
# EXPECT: true
# EXPECT: 2024-03-16T10:30:00
# EXPECT: true
# EXPECT: Mar 15, 2024 10:30:00

import std.datetime

var dt = datetime.create(2024, 3, 15, 10, 30, 0)
println(datetime.to_iso(dt))

# Leap year
println(datetime.is_leap_year(2024))

# Add days
var tomorrow = datetime.add_days(dt, 1)
println(datetime.to_iso(tomorrow))

# Parse ISO
var parsed = datetime.parse_iso("2024-03-15T10:30:00")
println(datetime.equal(dt, parsed))

# Human-readable
println(datetime.to_string(dt))
