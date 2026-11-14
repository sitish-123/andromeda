use crate::{coef, db_to_lin};

const STEP: usize = 32;
/// Below this program level the gain holds, so silence and room noise are never pumped up.
const GATE_DB: f32 = -60.0;

/// Slow automatic gain toward a target level, like Dolby's volume leveler.
/// `amount` 0..10 scales the correction; 0 is bypass.
pub struct Leveler {
    fs: f32,
    power: f32,
    gain_db: f32,
    pub target_db: f32,
    pub amount: f32,
    pub max_gain_db: f32,
}

const MEASURE_MS: f32 = 400.0;
const DOWN_MS: f32 = 300.0;
const UP_MS: f32 = 2000.0;

impl Leveler {
    pub fn new(fs: f32) -> Self {
        Self { fs, power: 0.0, gain_db: 0.0, target_db: -20.0, amount: 0.0, max_gain_db: 12.0 }
    }

    pub fn process(&mut self, l: &mut [f32], r: &mut [f32]) {
        let meas = coef(MEASURE_MS, self.fs);
        #[allow(clippy::cast_precision_loss)]
        let (down, up) = (coef(DOWN_MS / STEP as f32, self.fs), coef(UP_MS / STEP as f32, self.fs));
        for (cl, cr) in l.chunks_mut(STEP).zip(r.chunks_mut(STEP)) {
            for (a, b) in cl.iter().zip(cr.iter()) {
                let e = f32::midpoint(a * a, b * b);
                self.power = e + (self.power - e) * meas;
            }
            let level = 10.0 * (self.power + 1e-12).log10() + 3.0103;
            let from = db_to_lin(self.gain_db);
            if level > GATE_DB {
                let want = ((self.target_db - level) * self.amount / 10.0).clamp(-self.max_gain_db, self.max_gain_db);
                let c = if want < self.gain_db { down } else { up };
                self.gain_db = want + (self.gain_db - want) * c;
            }
            let to = db_to_lin(self.gain_db);
            #[allow(clippy::cast_precision_loss)]
            let step = (to - from) / cl.len() as f32;
            for (i, (a, b)) in cl.iter_mut().zip(cr.iter_mut()).enumerate() {
                #[allow(clippy::cast_precision_loss)]
                let g = from + step * (i + 1) as f32;
                *a *= g;
                *b *= g;
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn quiet_program_rises_to_max_gain() {
        let mut lev = Leveler::new(48_000.0);
        lev.amount = 10.0;
        #[allow(clippy::cast_precision_loss)]
        let x: Vec<f32> =
            (0..48_000 * 10).map(|i| 0.01 * (std::f32::consts::TAU * 300.0 * i as f32 / 48_000.0).sin()).collect();
        let (mut l, mut r) = (x.clone(), x);
        for (cl, cr) in l.chunks_mut(480).zip(r.chunks_mut(480)) {
            lev.process(cl, cr);
        }
        let peak = l[48_000 * 9..].iter().fold(0f32, |m, s| m.max(s.abs()));
        let gain = 20.0 * (peak / 0.01).log10();
        assert!((11.0..=12.05).contains(&gain), "gain {gain} dB");
    }
}
