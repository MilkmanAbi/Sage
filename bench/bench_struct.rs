use std::time::Instant;

struct Vec2 { x: f64, y: f64 }
impl Vec2 {
    fn mag_sq(&self) -> f64 { self.x * self.x + self.y * self.y }
}

fn main() {
    let start = Instant::now();
    let mut total: f64 = 0.0;
    for i in 0..50000 {
        let v = Vec2 { x: i as f64, y: (i + 1) as f64 };
        total += v.mag_sq();
    }
    let elapsed = start.elapsed();
    println!("{}", total as i64);
    println!("time: {:.6}s", elapsed.as_secs_f64());
}
