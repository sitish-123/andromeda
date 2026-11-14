use std::collections::VecDeque;

use crate::{coef, db_to_lin};

pub const LOOKAHEAD_MS: f32 = 5.0;

/// Stereo-linked lookahead peak limiter. Output never exceeds the ceiling.
///
/// The output is the input delayed by `len` samples. The gain at the output sample is the mean
/// of the last `len` released gains; each released gain is at most the minimum required gain over
/// a window of `len + 1` samples, which always contains the output sample.
pub struct Limiter {
    fs: f32,
    len: usize,
    pos: usize,
    n: u64,
    delay: Vec<[f32; 2]>,
    gains: Vec<f32>,
    sum: f64,
    released: f32,
    /// (sample index, required gain), increasing gain from the front.
    minq: VecDeque<(u64, f32)>,
    pub input_gain_db: f32,
    pub ceiling_db: f32,
    pub release_ms: f32,
}

impl Limiter {
    pub fn new(fs: f32) -> Self {
        #[allow(clippy::cast_possible_truncation, clippy::cast_sign_loss)]
        let len = ((LOOKAHEAD_MS * 0.001 * fs).round() as usize).max(1);
        Self {
            fs,
            len,
            pos: 0,
            n: 0,
            delay: vec![[0.0; 2]; len],
            gains: vec![1.0; len],
            #[allow(clippy::cast_precision_loss)]
            sum: len as f64,
            released: 1.0,
            minq: VecDeque::with_capacity(len + 2),
            input_gain_db: 0.0,
            ceiling_db: -1.0,
            release_ms: 80.0,
        }
    }

    /// Latency in samples.
    pub fn latency(&self) -> usize {
        self.len
    }

    pub fn process(&mut self, l: &mut [f32], r: &mut [f32]) {
        let gin = db_to_lin(self.input_gain_db);
        let ceil = db_to_lin(self.ceiling_db);
        let rel = coef(self.release_ms, self.fs);
        let window = self.len as u64 + 1;
        #[allow(clippy::cast_precision_loss)]
        let len = self.len as f64;
        for (xl, xr) in l.iter_mut().zip(r.iter_mut()) {
            let x = [*xl * gin, *xr * gin];
            let peak = x[0].abs().max(x[1].abs());
            let need = if peak > ceil { ceil / peak } else { 1.0 };
            while self.minq.back().is_some_and(|&(_, g)| g >= need) {
                self.minq.pop_back();
            }
            self.minq.push_back((self.n, need));
            while self.minq.front().is_some_and(|&(i, _)| i + window <= self.n) {
                self.minq.pop_front();
            }
            let held = self.minq.front().map_or(1.0, |&(_, g)| g);
            self.released = if held < self.released { held } else { held + (self.released - held) * rel };
            self.sum += f64::from(self.released) - f64::from(self.gains[self.pos]);
            self.gains[self.pos] = self.released;
            #[allow(clippy::cast_possible_truncation)]
            let g = ((self.sum / len) as f32).min(1.0);
            let out = self.delay[self.pos];
            self.delay[self.pos] = x;
            *xl = out[0] * g;
            *xr = out[1] * g;
            self.pos = (self.pos + 1) % self.len;
            self.n += 1;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tone(amp: f32, hz: f32, n: usize) -> Vec<f32> {
        #[allow(clippy::cast_precision_loss)]
        (0..n).map(|i| amp * (std::f32::consts::TAU * hz * i as f32 / 48_000.0).sin()).collect()
    }

    #[test]
    fn overdriven_input_stays_under_ceiling() {
        let mut lim = Limiter::new(48_000.0);
        let mut l = tone(4.0, 1000.0, 48_000);
        // A burst that jumps from silence to +18 dB in one sample.
        l[20_000..20_100].iter_mut().for_each(|s| *s = 8.0);
        let mut r = tone(4.0, 57.0, 48_000);
        for (cl, cr) in l.chunks_mut(256).zip(r.chunks_mut(256)) {
            lim.process(cl, cr);
        }
        let ceil = db_to_lin(-1.0);
        let peak = l.iter().chain(&r).fold(0f32, |m, s| m.max(s.abs()));
        assert!(peak <= ceil * 1.0001, "peak {peak} over ceiling {ceil}");
        assert!(peak > ceil * 0.9, "limiter squashed too far: peak {peak}");
    }

    #[test]
    fn quiet_input_passes_unchanged_after_latency() {
        let mut lim = Limiter::new(48_000.0);
        let x = tone(0.1, 440.0, 4800);
        let (mut l, mut r) = (x.clone(), x.clone());
        lim.process(&mut l, &mut r);
        let d = lim.latency();
        assert!(l[d..].iter().zip(&x).all(|(y, x)| (y - x).abs() < 1e-6));
    }
}
