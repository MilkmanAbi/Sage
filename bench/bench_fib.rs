use std::time::Instant;

fn fib(n: i32) -> i32 {
    if n <= 1 { return n; }
    fib(n - 1) + fib(n - 2)
}

fn main() {
    let start = Instant::now();
    let result = fib(30);
    let elapsed = start.elapsed();
    println!("{}", result);
    println!("time: {:.6}s", elapsed.as_secs_f64());
}
