use std::collections::VecDeque;

use crate::biquad::{Biquad, Coeffs};
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
    /// Crossover in Hz for the two-band compressor ahead of the limiter; 0 turns it off.
    pub split_hz: f32,
    split: SplitComp,
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
            split_hz: 0.0,
            split: SplitComp::new(fs),
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
        if self.split_hz > 0.0 {
            self.split.set_split(self.split_hz);
        }
        for (xl, xr) in l.iter_mut().zip(r.iter_mut()) {
            let mut x = [*xl * gin, *xr * gin];
            if self.split_hz > 0.0 {
                x = self.split.run(x, ceil);
            }
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

/// One band of [`SplitComp`]: stereo-linked peak compressor, soft knee.
struct Band {
    env: f32,
    att: f32,
    rel: f32,
    threshold_db: f32,
}

const RATIO: f32 = 3.0;
const KNEE_DB: f32 = 6.0;

impl Band {
    fn new(fs: f32, attack_ms: f32, release_ms: f32, threshold_db: f32) -> Self {
        Self { env: 0.0, att: coef(attack_ms, fs), rel: coef(release_ms, fs), threshold_db }
    }

    fn gain(&mut self, peak: f32, ceil: f32) -> f32 {
        let c = if peak > self.env { self.att } else { self.rel };
        self.env = peak + (self.env - peak) * c;
        // Below the knee the band needs no log: most samples of normal program take this path.
        let knee_start = ceil * db_to_lin(self.threshold_db - KNEE_DB / 2.0);
        if self.env <= knee_start {
            return 1.0;
        }
        let over = 20.0 * (self.env / ceil).log10() - self.threshold_db;
        let cut = if over < KNEE_DB / 2.0 {
            (1.0 - 1.0 / RATIO) * (over + KNEE_DB / 2.0).powi(2) / (2.0 * KNEE_DB)
        } else {
            (1.0 - 1.0 / RATIO) * over
        };
        db_to_lin(-cut)
    }
}

/// Linkwitz-Riley crossover (low and high sum flat) with one compressor per band, so bass
/// peaks no longer pull the mids and highs down in the broadband limiter. Thresholds are dB
/// under the limiter ceiling.
struct SplitComp {
    fs: f32,
    hz: f32,
    lp: [[Biquad; 2]; 2],
    hp: [[Biquad; 2]; 2],
    low: Band,
    high: Band,
}

impl SplitComp {
    fn new(fs: f32) -> Self {
        let id = Biquad::new(Coeffs::lowpass(fs, 200.0, std::f32::consts::FRAC_1_SQRT_2));
        Self {
            fs,
            hz: 0.0,
            lp: [[id; 2]; 2],
            hp: [[id; 2]; 2],
            low: Band::new(fs, 10.0, 150.0, -9.0),
            high: Band::new(fs, 2.0, 100.0, -1.0),
        }
    }

    fn set_split(&mut self, hz: f32) {
        #[allow(clippy::float_cmp)]
        if hz == self.hz {
            return;
        }
        self.hz = hz;
        let q = std::f32::consts::FRAC_1_SQRT_2;
        let (lp, hp) = (Coeffs::lowpass(self.fs, hz, q), Coeffs::highpass(self.fs, hz, q));
        self.lp.iter_mut().flatten().for_each(|b| b.set(lp));
        self.hp.iter_mut().flatten().for_each(|b| b.set(hp));
    }

    #[inline]
    fn run(&mut self, x: [f32; 2], ceil: f32) -> [f32; 2] {
        let mut lo = [0.0; 2];
        let mut hi = [0.0; 2];
        for ch in 0..2 {
            let [a, b] = &mut self.lp[ch];
            lo[ch] = b.run(a.run(x[ch]));
            let [a, b] = &mut self.hp[ch];
            hi[ch] = b.run(a.run(x[ch]));
        }
        let gl = self.low.gain(lo[0].abs().max(lo[1].abs()), ceil);
        let gh = self.high.gain(hi[0].abs().max(hi[1].abs()), ceil);
        [lo[0] * gl + hi[0] * gh, lo[1] * gl + hi[1] * gh]
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

    /// Level of `hz` in `x` (amplitude), by single-bin DFT over whole cycles.
    fn amp_at(x: &[f32], hz: f32) -> f32 {
        let (mut re, mut im) = (0f32, 0f32);
        for (i, s) in x.iter().enumerate() {
            #[allow(clippy::cast_precision_loss)]
            let ph = std::f32::consts::TAU * hz * i as f32 / 48_000.0;
            re += s * ph.cos();
            im += s * ph.sin();
        }
        #[allow(clippy::cast_precision_loss)]
        let n = x.len() as f32;
        2.0 * re.hypot(im) / n
    }

    #[test]
    fn loud_bass_does_not_duck_the_mids_with_split() {
        let mids = |split_hz: f32| {
            let mut lim = Limiter::new(48_000.0);
            lim.split_hz = split_hz;
            let bass = tone(2.0, 60.0, 96_000);
            let mid = tone(0.1, 1000.0, 96_000);
            let mut l: Vec<f32> = bass.iter().zip(&mid).map(|(a, b)| a + b).collect();
            let mut r = l.clone();
            for (cl, cr) in l.chunks_mut(256).zip(r.chunks_mut(256)) {
                lim.process(cl, cr);
            }
            let peak = l[48_000..].iter().fold(0f32, |m, s| m.max(s.abs()));
            assert!(peak <= db_to_lin(-1.0) * 1.0001);
            amp_at(&l[48_000..96_000], 1000.0)
        };
        let (plain, split) = (mids(0.0), mids(200.0));
        assert!(plain < 0.06, "broadband limiter should duck the 1 kHz tone: {plain}");
        assert!(split > 0.09, "split limiter kept 1 kHz at {split}, want ~0.1");
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
