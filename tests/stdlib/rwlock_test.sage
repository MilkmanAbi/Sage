gc_disable()
# EXPECT: true
# EXPECT: 2
# EXPECT: false
# EXPECT: true

import std.rwlock

var rw = rwlock.create()
rwlock.read_lock(rw)
rwlock.read_lock(rw)
println(rwlock.is_read_locked(rw))
println(rwlock.reader_count(rw))

# Can't write lock while readers
println(rwlock.try_write_lock(rw))

rwlock.read_unlock(rw)
rwlock.read_unlock(rw)
println(rwlock.try_write_lock(rw))
