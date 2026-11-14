use crate::biquad::{Biquad, Coeffs};
use crate::coef;

const BUTTERWORTH_Q: f32 = std::f32::consts::FRAC_1_SQRT_2;

/// Psychoacoustic bass: harmonics of the 35-160 Hz band, played at 94-469 Hz where small
/// speakers work, so the ear hears the missing fundamental. A high-pass removes the
/// lows the speaker cannot play. Ranges follow Dolby's virtual-bass fields.
pub struct VirtualBass {
    fs: f32,
    src: [Biquad; 2],
    mix: [Biquad; 4],
    cut: [[Biquad; 2]; 2],
    env: f32,
    cut_hz: f32,
    /// 0..10; 0 adds nothing.
    pub amount: f32,
    /// High-pass on the main signal in Hz; 0 is off.
    pub highpass_hz: f32,
}

const SRC_HZ: (f32, f32) = (35.0, 160.0);
const MIX_HZ: (f32, f32) = (94.0, 469.0);

impl VirtualBass {
    pub fn new(fs: f32) -> Self {
        let hp = |f| Biquad::new(Coeffs::highpass(fs, f, BUTTERWORTH_Q));
        let lp = |f| Biquad::new(Coeffs::lowpass(fs, f, BUTTERWORTH_Q));
        Self {
            fs,
            src: [hp(SRC_HZ.0), lp(SRC_HZ.1)],
            mix: [hp(MIX_HZ.0), hp(MIX_HZ.0), lp(MIX_HZ.1), lp(MIX_HZ.1)],
            cut: [[hp(80.0), hp(80.0)]; 2],
            env: 0.0,
            cut_hz: 80.0,
            amount: 0.0,
            highpass_hz: 0.0,
        }
    }

    #[allow(clippy::many_single_char_names)]
    pub fn process(&mut self, l: &mut [f32], r: &mut [f32]) {
        if self.highpass_hz > 0.0 && (self.highpass_hz - self.cut_hz).abs() > 0.5 {
            self.cut_hz = self.highpass_hz;
            let c = Coeffs::highpass(self.fs, self.cut_hz, BUTTERWORTH_Q);
            self.cut.iter_mut().flatten().for_each(|f| f.set(c));
        }
        let (att, rel) = (coef(5.0, self.fs), coef(60.0, self.fs));
        let gain = 0.2 * self.amount;
        for (a, b) in l.iter_mut().zip(r.iter_mut()) {
            let low = self.src.iter_mut().fold(f32::midpoint(*a, *b), |x, f| f.run(x));
            let mag = low.abs();
            let c = if mag > self.env { att } else { rel };
            self.env = mag + (self.env - mag) * c;
            // Level-independent shape: even (|s|) and odd (s|s|) harmonics of the normalized band.
            let s = (low / (self.env + 1e-6)).clamp(-1.0, 1.0);
            let h = self.env * (0.6 * s.abs() + 0.4 * s * s.abs());
            let add = gain * self.mix.iter_mut().fold(h, |x, f| f.run(x));
            if self.highpass_hz > 0.0 {
                *a = self.cut[0].iter_mut().fold(*a, |x, f| f.run(x));
                *b = self.cut[1].iter_mut().fold(*b, |x, f| f.run(x));
            }
            *a += add;
            *b += add;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Power at `hz` in the second half of `x` (single-bin DFT).
    fn power_at(x: &[f32], hz: f32) -> f32 {
        let tail = &x[x.len() / 2..];
        let (mut re, mut im) = (0f32, 0f32);
        for (i, s) in tail.iter().enumerate() {
            #[allow(clippy::cast_precision_loss)]
            let w = std::f32::consts::TAU * hz * i as f32 / 48_000.0;
            re += s * w.cos();
            im += s * w.sin();
        }
        re * re + im * im
    }

    fn run(amount: f32) -> Vec<f32> {
        let mut vb = VirtualBass::new(48_000.0);
        vb.amount = amount;
        #[allow(clippy::cast_precision_loss)]
        let x: Vec<f32> =
            (0..48_000).map(|i| 0.3 * (std::f32::consts::TAU * 60.0 * i as f32 / 48_000.0).sin()).collect();
        let (mut l, mut r) = (x.clone(), x);
        vb.process(&mut l, &mut r);
        l
    }

    #[test]
    fn deep_bass_gains_audible_harmonics() {
        let (off, on) = (run(0.0), run(10.0));
        for h in [120.0, 180.0, 240.0] {
            let gain_db = 10.0 * (power_at(&on, h) / power_at(&off, h).max(1e-9)).log10();
            assert!(gain_db > 20.0, "{h} Hz rose only {gain_db} dB");
        }
    }
}
