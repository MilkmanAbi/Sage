# EXPECT: submit_ok
# EXPECT: run_all_ok
# EXPECT: parallel_map_ok
# EXPECT: multi_arg_ok
# EXPECT: error_handling_ok
# EXPECT: future_ok
# EXPECT: PASS
from std.threadpool import create, submit, run_all, get_result, pool_stats, parallel_map
from std.threadpool import create_future, resolve, reject, is_resolved, future_value

# --- submit and run_all ---
var pool = create(4)
proc double(x):
    return x * 2
var id1 = submit(pool, double, [5])
var id2 = submit(pool, double, [10])
var st0 = pool_stats(pool)
if st0["queued"] == 2:
    run_all(pool)
    let r1 = get_result(pool, id1)
    let r2 = get_result(pool, id2)
    if r1 == 10 and r2 == 20:
        println("submit_ok")

# --- run_all clears queue ---
var pool2 = create(2)
proc add(a, b):
    return a + b
submit(pool2, add, [3, 4])
run_all(pool2)
var st2 = pool_stats(pool2)
if st2["queued"] == 0 and st2["completed"] == 1:
    println("run_all_ok")

# --- parallel_map ---
var pool3 = create(4)
proc square(x):
    return x * x
var results = parallel_map(pool3, square, [1, 2, 3, 4, 5])
if results.length == 5:
    if results[0] == 1 and results[1] == 4 and results[2] == 9 and results[3] == 16 and results[4] == 25:
        println("parallel_map_ok")

# --- 4-arg task (new fix) ---
var pool4 = create(1)
proc sum4(a, b, c, d):
    return a + b + c + d
var id4 = submit(pool4, sum4, [1, 2, 3, 4])
run_all(pool4)
var r4 = get_result(pool4, id4)
if r4 == 10:
    println("multi_arg_ok")

# --- error handling in tasks ---
var pool5 = create(1)
proc bad_fn():
    raise "intentional error"
var bad_id = submit(pool5, bad_fn, [])
run_all(pool5)
var st5 = pool_stats(pool5)
if st5["failed"] == 1 and st5["completed"] == 0:
    println("error_handling_ok")

# --- Future / Promise ---
var f = create_future()
if is_resolved(f) == false:
    resolve(f, 99)
    if is_resolved(f) == true:
        let val = future_value(f)
        if val == 99:
            # rejected future raises on future_value
            let f2 = create_future()
            reject(f2, "oops")
            var caught = false
            try:
                future_value(f2)
            catch e:
                caught = true
            if caught:
                println("future_ok")

println("PASS")
