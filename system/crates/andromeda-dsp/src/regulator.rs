use crate::biquad::{Biquad, Coeffs};
use crate::coef;

/// Dolby DAX3 band centres at 48 kHz.
pub const BAND_HZ: [f32; BANDS] = [
    47., 141., 234., 328., 469., 656., 844., 1031., 1313., 1688., 2250., 3000., 3750., 4688., 5813., 7125., 9000.,
    11250., 13875., 19688.,
];
pub const BANDS: usize = 20;
/// Gains are recomputed once per this many samples.
const STEP: usize = 32;

/// Per-band speaker protection as a dynamic EQ.
///
/// Each band measures its own level (band-pass power, as peak dBFS of a sine) and cuts a peaking
/// filter at the band centre. Below `low` nothing happens; between `low` and `high` a quadratic
/// knee; above `high` the band output is held at `(low + high) / 2`. Both channels take the larger cut.
pub struct Regulator {
    fs: f32,
    bands: Vec<Band>,
    pub high_db: [f32; BANDS],
    pub low_db: [f32; BANDS],
    pub attack_ms: f32,
    pub release_ms: f32,
}

struct Band {
    hz: f32,
    q: f32,
    detect: [Biquad; 2],
    power: [f32; 2],
    eq: [Biquad; 2],
    cut_db: f32,
}

impl Regulator {
    pub fn new(fs: f32) -> Self {
        let bands = (0..BANDS)
            .filter(|&i| BAND_HZ[i] < 0.45 * fs)
            .map(|i| {
                let f = BAND_HZ[i];
                let lo = if i > 0 { BAND_HZ[i - 1] } else { f * f / BAND_HZ[1] };
                let hi = if i + 1 < BANDS { BAND_HZ[i + 1] } else { f * f / BAND_HZ[i - 1] };
                let bw = 2f32.powf((hi / lo).log2() / 2.0);
                let q = bw.sqrt() / (bw - 1.0);
                Band {
                    hz: f,
                    q,
                    detect: [Biquad::new(Coeffs::bandpass(fs, f, q)); 2],
                    power: [0.0; 2],
                    eq: [Biquad::new(Coeffs::peaking(fs, f, q, 0.0)); 2],
                    cut_db: 0.0,
                }
            })
            .collect();
        Self { fs, bands, high_db: [0.0; BANDS], low_db: [-12.0; BANDS], attack_ms: 5.0, release_ms: 150.0 }
    }

    #[allow(clippy::many_single_char_names)]
    pub fn process(&mut self, l: &mut [f32], r: &mut [f32]) {
        let att = coef(self.attack_ms, self.fs);
        let rel = coef(self.release_ms, self.fs);
        for (cl, cr) in l.chunks_mut(STEP).zip(r.chunks_mut(STEP)) {
            for (i, band) in self.bands.iter_mut().enumerate() {
                let mut level = f32::MIN;
                for (ch, block) in [&*cl, &*cr].into_iter().enumerate() {
                    let p = &mut band.power[ch];
                    for &x in block {
                        let y = band.detect[ch].run(x);
                        let e = y * y;
                        let a = if e > *p { att } else { rel };
                        *p = e + (*p - e) * a;
                    }
                    level = level.max(10.0 * (*p + 1e-12).log10() + 3.0103);
                }
                let cut = knee_cut(level, self.low_db[i], self.high_db[i]);
                if (cut - band.cut_db).abs() > 0.01 {
                    band.cut_db = cut;
                    let c = Coeffs::peaking(self.fs, band.hz, band.q, cut);
                    band.eq.iter_mut().for_each(|f| f.set(c));
                }
                for (ch, block) in [&mut *cl, &mut *cr].into_iter().enumerate() {
                    for x in block.iter_mut() {
                        *x = band.eq[ch].run(*x);
                    }
                }
            }
        }
    }
}

/// Gain change in dB (≤ 0) for a band at `level` dB.
fn knee_cut(level: f32, low: f32, high: f32) -> f32 {
    let k = high - low;
    if k <= 0.0 {
        (high - level).min(0.0)
    } else if level <= low {
        0.0
    } else if level < high {
        -(level - low).powi(2) / (2.0 * k)
    } else {
        low + k / 2.0 - level
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// UX3405CA music profile, converted from 1/16 dB.
    fn ux3405() -> Regulator {
        let mut reg = Regulator::new(48_000.0);
        let high = [0., 0., -198., -517., -490., -440., 0., 0., 0., 0., 0., 0., 0., 0., 0., 0., 0., 0., 0., 0.];
        let low = [
            -192., -192., -390., -709., -682., -632., -192., -192., -192., -192., -192., -192., -192., -192., -192.,
            -192., -192., -192., -192., -192.,
        ];
        for i in 0..BANDS {
            reg.high_db[i] = high[i] / 16.0;
            reg.low_db[i] = low[i] / 16.0;
        }
        reg
    }

    /// Output RMS in dB over the last 0.5 s of a 2 s tone.
    fn rms_out_db(reg: &mut Regulator, amp: f32, hz: f32) -> f32 {
        #[allow(clippy::cast_precision_loss)]
        let x: Vec<f32> = (0..96_000).map(|i| amp * (std::f32::consts::TAU * hz * i as f32 / 48_000.0).sin()).collect();
        let (mut l, mut r) = (x.clone(), x);
        for (cl, cr) in l.chunks_mut(512).zip(r.chunks_mut(512)) {
            reg.process(cl, cr);
        }
        let tail = &l[72_000..];
        #[allow(clippy::cast_precision_loss)]
        let ms = tail.iter().map(|s| s * s).sum::<f32>() / tail.len() as f32;
        10.0 * ms.log10()
    }

    #[test]
    fn loud_tone_in_protected_band_is_cut() {
        let out = rms_out_db(&mut ux3405(), 0.5, 328.0);
        let input = 20.0 * 0.5f32.log10() - 3.0103;
        assert!(input - out > 20.0, "cut only {} dB", input - out);
    }

    #[test]
    fn quiet_tone_in_open_band_is_untouched() {
        let out = rms_out_db(&mut ux3405(), 0.1, 2000.0);
        let input = 20.0 * 0.1f32.log10() - 3.0103;
        assert!((input - out).abs() < 0.3, "changed by {} dB", input - out);
    }
}
