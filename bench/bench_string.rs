use std::time::Instant;

fn main() {
    let start = Instant::now();
    let mut result = String::new();
    for i in 0..10000 {
        result.push_str(&i.to_string());
    }
    let length = result.len();
    let elapsed = start.elapsed();
    println!("{}", length);
    println!("time: {:.6}s", elapsed.as_secs_f64());
}
