use std::f64::consts::PI;

#[derive(Clone, Copy)]
pub struct Coeffs {
    b0: f64,
    b1: f64,
    b2: f64,
    a1: f64,
    a2: f64,
}

impl Coeffs {
    pub fn peaking(fs: f32, f: f32, q: f32, gain_db: f32) -> Self {
        let a = 10f64.powf(f64::from(gain_db) / 40.0);
        let (cos, alpha) = Self::w(fs, f, q);
        Self::norm(1.0 + alpha * a, -2.0 * cos, 1.0 - alpha * a, 1.0 + alpha / a, -2.0 * cos, 1.0 - alpha / a)
    }

    /// Band-pass with 0 dB gain at `f`.
    pub fn bandpass(fs: f32, f: f32, q: f32) -> Self {
        let (cos, alpha) = Self::w(fs, f, q);
        Self::norm(alpha, 0.0, -alpha, 1.0 + alpha, -2.0 * cos, 1.0 - alpha)
    }

    pub fn highpass(fs: f32, f: f32, q: f32) -> Self {
        let (cos, alpha) = Self::w(fs, f, q);
        let b = f64::midpoint(1.0, cos);
        Self::norm(b, -2.0 * b, b, 1.0 + alpha, -2.0 * cos, 1.0 - alpha)
    }

    pub fn lowpass(fs: f32, f: f32, q: f32) -> Self {
        let (cos, alpha) = Self::w(fs, f, q);
        Self::norm((1.0 - cos) / 2.0, 1.0 - cos, (1.0 - cos) / 2.0, 1.0 + alpha, -2.0 * cos, 1.0 - alpha)
    }

    fn w(fs: f32, f: f32, q: f32) -> (f64, f64) {
        let w0 = 2.0 * PI * f64::from(f) / f64::from(fs);
        (w0.cos(), w0.sin() / (2.0 * f64::from(q)))
    }

    fn norm(b0: f64, b1: f64, b2: f64, a0: f64, a1: f64, a2: f64) -> Self {
        Self { b0: b0 / a0, b1: b1 / a0, b2: b2 / a0, a1: a1 / a0, a2: a2 / a0 }
    }
}

/// Transposed direct form II; f64 state keeps the 47 Hz band stable.
#[derive(Clone, Copy)]
pub struct Biquad {
    c: Coeffs,
    z1: f64,
    z2: f64,
}

impl Biquad {
    pub fn new(c: Coeffs) -> Self {
        Self { c, z1: 0.0, z2: 0.0 }
    }

    pub fn set(&mut self, c: Coeffs) {
        self.c = c;
    }

    #[inline]
    pub fn run(&mut self, x: f32) -> f32 {
        let x = f64::from(x);
        let y = self.c.b0 * x + self.z1;
        self.z1 = self.c.b1 * x - self.c.a1 * y + self.z2;
        self.z2 = self.c.b2 * x - self.c.a2 * y;
        #[allow(clippy::cast_possible_truncation)]
        let y = y as f32;
        y
    }
}
