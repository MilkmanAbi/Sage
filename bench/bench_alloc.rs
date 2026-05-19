use std::time::Instant;

fn main() {
    let start = Instant::now();
    let mut total: i64 = 0;
    for i in 0..100000i64 {
        let arr = vec![i, i+1, i+2, i+3, i+4];
        total += arr[2];
    }
    let elapsed = start.elapsed();
    println!("{}", total);
    println!("time: {:.6}s", elapsed.as_secs_f64());
}
