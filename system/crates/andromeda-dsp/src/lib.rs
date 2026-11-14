//! Speaker DSP for the `PipeWire` filter-chain, as a LADSPA library: a lookahead limiter,
//! a multiband regulator (per-band speaker protection), a volume leveler
//! and psychoacoustic bass.
//! Every processor works in place on one stereo block and allocates only in `new`.

pub mod bass;
mod biquad;
pub mod ladspa;
pub mod leveler;
pub mod limiter;
pub mod regulator;

pub(crate) fn db_to_lin(db: f32) -> f32 {
    10f32.powf(db / 20.0)
}

/// One-pole smoothing coefficient for a time constant in ms.
pub(crate) fn coef(ms: f32, fs: f32) -> f32 {
    if ms <= 0.0 { 0.0 } else { (-1.0 / (ms * 0.001 * fs)).exp() }
}
