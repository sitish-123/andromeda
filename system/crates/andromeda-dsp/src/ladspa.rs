//! LADSPA 1.1 ABI. Plugins (labels): `limiter`, `regulator`, `leveler`, `bass`; each has ports
//! `In L`, `In R`, `Out L`, `Out R`, then its controls.
#![allow(unsafe_code)]

use std::ffi::{CString, c_char, c_int, c_ulong, c_void};
use std::ptr;
use std::sync::OnceLock;

use crate::bass::VirtualBass;
use crate::leveler::Leveler;
use crate::limiter::Limiter;
use crate::regulator::{BANDS, Regulator};

const PORT_INPUT: c_int = 1;
const PORT_OUTPUT: c_int = 2;
const PORT_CONTROL: c_int = 4;
const PORT_AUDIO: c_int = 8;
const HINT_BOUNDED: c_int = 0x1 | 0x2;
const PROP_HARD_RT: c_int = 1 | 4;
/// Private-range ids; no clash with the registered LADSPA ids below 4096.
const ID_BASE: c_ulong = 0x0041_4E44;
const AUDIO_PORTS: usize = 4;

#[repr(C)]
pub struct PortRangeHint {
    hint: c_int,
    lower: f32,
    upper: f32,
}

type Handle = *mut c_void;

#[repr(C)]
pub struct Descriptor {
    pub unique_id: c_ulong,
    pub label: *const c_char,
    properties: c_int,
    name: *const c_char,
    maker: *const c_char,
    copyright: *const c_char,
    pub port_count: c_ulong,
    port_descriptors: *const c_int,
    pub port_names: *const *const c_char,
    port_range_hints: *const PortRangeHint,
    implementation_data: *mut c_void,
    pub instantiate: Option<unsafe extern "C" fn(*const Descriptor, c_ulong) -> Handle>,
    pub connect_port: Option<unsafe extern "C" fn(Handle, c_ulong, *mut f32)>,
    pub activate: Option<unsafe extern "C" fn(Handle)>,
    pub run: Option<unsafe extern "C" fn(Handle, c_ulong)>,
    run_adding: Option<unsafe extern "C" fn(Handle, c_ulong)>,
    set_run_adding_gain: Option<unsafe extern "C" fn(Handle, f32)>,
    deactivate: Option<unsafe extern "C" fn(Handle)>,
    pub cleanup: Option<unsafe extern "C" fn(Handle)>,
}

struct Control {
    name: String,
    lower: f32,
    upper: f32,
    default: f32,
}

fn control(name: &str, lower: f32, upper: f32, default: f32) -> Control {
    Control { name: name.to_owned(), lower, upper, default }
}

#[derive(Clone, Copy)]
enum Kind {
    Limiter,
    Regulator,
    Leveler,
    Bass,
}

const KINDS: [Kind; 4] = [Kind::Limiter, Kind::Regulator, Kind::Leveler, Kind::Bass];

impl Kind {
    fn label(self) -> &'static str {
        match self {
            Kind::Limiter => "limiter",
            Kind::Regulator => "regulator",
            Kind::Leveler => "leveler",
            Kind::Bass => "bass",
        }
    }

    fn controls(self) -> Vec<Control> {
        match self {
            Kind::Limiter => vec![
                control("Input gain", -24.0, 24.0, 0.0),
                control("Ceiling", -24.0, 0.0, -1.0),
                control("Release", 1.0, 1000.0, 80.0),
            ],
            Kind::Regulator => (0..BANDS)
                .map(|i| control(&format!("High {i}"), -96.0, 0.0, 0.0))
                .chain((0..BANDS).map(|i| control(&format!("Low {i}"), -96.0, 0.0, -12.0)))
                .chain([
                    control("Attack", 0.1, 100.0, 5.0),
                    control("Release", 1.0, 2000.0, 150.0),
                    control("Max cut", 0.0, 96.0, 6.0),
                ])
                .collect(),
            Kind::Leveler => vec![
                control("Target", -60.0, 0.0, -20.0),
                control("Amount", 0.0, 10.0, 0.0),
                control("Max gain", 0.0, 24.0, 12.0),
            ],
            Kind::Bass => vec![control("Amount", 0.0, 10.0, 0.0), control("Highpass", 0.0, 200.0, 0.0)],
        }
    }
}

enum Dsp {
    Limiter(Limiter),
    Regulator(Box<Regulator>),
    Leveler(Leveler),
    Bass(Box<VirtualBass>),
}

impl Dsp {
    fn new(kind: Kind, fs: f32) -> Self {
        match kind {
            Kind::Limiter => Dsp::Limiter(Limiter::new(fs)),
            Kind::Regulator => Dsp::Regulator(Box::new(Regulator::new(fs))),
            Kind::Leveler => Dsp::Leveler(Leveler::new(fs)),
            Kind::Bass => Dsp::Bass(Box::new(VirtualBass::new(fs))),
        }
    }

    fn run(&mut self, c: &[f32], l: &mut [f32], r: &mut [f32]) {
        match self {
            Dsp::Limiter(d) => {
                (d.input_gain_db, d.ceiling_db, d.release_ms) = (c[0], c[1], c[2]);
                d.process(l, r);
            }
            Dsp::Regulator(d) => {
                d.high_db.copy_from_slice(&c[..BANDS]);
                d.low_db.copy_from_slice(&c[BANDS..2 * BANDS]);
                (d.attack_ms, d.release_ms, d.max_cut_db) = (c[2 * BANDS], c[2 * BANDS + 1], c[2 * BANDS + 2]);
                d.process(l, r);
            }
            Dsp::Leveler(d) => {
                (d.target_db, d.amount, d.max_gain_db) = (c[0], c[1], c[2]);
                d.process(l, r);
            }
            Dsp::Bass(d) => {
                (d.amount, d.highpass_hz) = (c[0], c[1]);
                d.process(l, r);
            }
        }
    }
}

struct Instance {
    kind: Kind,
    fs: f32,
    dsp: Dsp,
    ports: Vec<*mut f32>,
    defaults: Vec<f32>,
    values: Vec<f32>,
}

/// Owns everything the descriptor points into; heap buffers do not move with the struct.
struct Entry {
    desc: Descriptor,
    _strings: Vec<CString>,
    _names: Vec<*const c_char>,
    _ports: Vec<c_int>,
    _hints: Vec<PortRangeHint>,
}

// SAFETY: built once, never mutated after; the pointers refer to the entry's own heap data.
unsafe impl Send for Entry {}
unsafe impl Sync for Entry {}

fn cstr(s: &str) -> CString {
    CString::new(s).unwrap_or_default()
}

fn entry(index: usize, kind: Kind) -> Entry {
    let controls = kind.controls();
    let mut strings = vec![
        cstr(kind.label()),
        cstr(&format!("Andromeda {}", kind.label())),
        cstr("Andromeda"),
        cstr("GPL-3.0-or-later"),
    ];
    let audio = [("In L", PORT_INPUT), ("In R", PORT_INPUT), ("Out L", PORT_OUTPUT), ("Out R", PORT_OUTPUT)];
    let mut ports = Vec::new();
    let mut hints = Vec::new();
    for (name, dir) in audio {
        strings.push(cstr(name));
        ports.push(dir | PORT_AUDIO);
        hints.push(PortRangeHint { hint: 0, lower: 0.0, upper: 0.0 });
    }
    for c in &controls {
        strings.push(cstr(&c.name));
        ports.push(PORT_INPUT | PORT_CONTROL);
        hints.push(PortRangeHint { hint: HINT_BOUNDED, lower: c.lower, upper: c.upper });
    }
    let names: Vec<*const c_char> = strings[4..].iter().map(|s| s.as_ptr()).collect();
    let desc = Descriptor {
        unique_id: ID_BASE + index as c_ulong,
        label: strings[0].as_ptr(),
        properties: PROP_HARD_RT,
        name: strings[1].as_ptr(),
        maker: strings[2].as_ptr(),
        copyright: strings[3].as_ptr(),
        port_count: ports.len() as c_ulong,
        port_descriptors: ports.as_ptr(),
        port_names: names.as_ptr(),
        port_range_hints: hints.as_ptr(),
        implementation_data: ptr::null_mut(),
        instantiate: Some(instantiate),
        connect_port: Some(connect_port),
        activate: Some(activate),
        run: Some(run),
        run_adding: None,
        set_run_adding_gain: None,
        deactivate: None,
        cleanup: Some(cleanup),
    };
    Entry { desc, _strings: strings, _names: names, _ports: ports, _hints: hints }
}

fn entries() -> &'static [Entry] {
    static ENTRIES: OnceLock<Vec<Entry>> = OnceLock::new();
    ENTRIES.get_or_init(|| KINDS.iter().enumerate().map(|(i, &k)| entry(i, k)).collect())
}

/// LADSPA entry point.
#[unsafe(no_mangle)]
pub extern "C" fn ladspa_descriptor(index: c_ulong) -> *const Descriptor {
    usize::try_from(index).ok().and_then(|i| entries().get(i)).map_or(ptr::null(), |e| &raw const e.desc)
}

unsafe extern "C" fn instantiate(desc: *const Descriptor, rate: c_ulong) -> Handle {
    // SAFETY: the host passes a descriptor that `ladspa_descriptor` returned.
    let id = unsafe { (*desc).unique_id };
    let Some(&kind) = usize::try_from(id.wrapping_sub(ID_BASE)).ok().and_then(|i| KINDS.get(i)) else {
        return ptr::null_mut();
    };
    #[allow(clippy::cast_precision_loss)]
    let fs = rate as f32;
    let defaults: Vec<f32> = kind.controls().iter().map(|c| c.default).collect();
    let inst = Instance {
        kind,
        fs,
        dsp: Dsp::new(kind, fs),
        ports: vec![ptr::null_mut(); AUDIO_PORTS + defaults.len()],
        values: defaults.clone(),
        defaults,
    };
    Box::into_raw(Box::new(inst)).cast()
}

unsafe extern "C" fn connect_port(h: Handle, port: c_ulong, data: *mut f32) {
    // SAFETY: `h` comes from `instantiate` and the host does not call this concurrently with `run`.
    let inst = unsafe { &mut *h.cast::<Instance>() };
    if let Some(p) = usize::try_from(port).ok().and_then(|p| inst.ports.get_mut(p)) {
        *p = data;
    }
}

unsafe extern "C" fn activate(h: Handle) {
    // SAFETY: as in `connect_port`.
    let inst = unsafe { &mut *h.cast::<Instance>() };
    inst.dsp = Dsp::new(inst.kind, inst.fs);
}

unsafe extern "C" fn run(h: Handle, count: c_ulong) {
    // SAFETY: as in `connect_port`.
    let inst = unsafe { &mut *h.cast::<Instance>() };
    let Ok(n) = usize::try_from(count) else { return };
    let [il, ir, ol, or] = [inst.ports[0], inst.ports[1], inst.ports[2], inst.ports[3]];
    if n == 0 || [il, ir, ol, or].iter().any(|p| p.is_null()) {
        return;
    }
    for (i, v) in inst.values.iter_mut().enumerate() {
        let p = inst.ports[AUDIO_PORTS + i];
        // SAFETY: a connected control port points at one f32 that the host keeps alive during `run`.
        *v = if p.is_null() { inst.defaults[i] } else { unsafe { *p } };
    }
    // SAFETY: each audio port holds `n` samples. Input and output may be the same buffer
    // (LADSPA in-place), so copy with overlap allowed and then touch only the outputs.
    let (l, r) = unsafe {
        ptr::copy(il, ol, n);
        ptr::copy(ir, or, n);
        (std::slice::from_raw_parts_mut(ol, n), std::slice::from_raw_parts_mut(or, n))
    };
    inst.dsp.run(&inst.values, l, r);
}

unsafe extern "C" fn cleanup(h: Handle) {
    // SAFETY: `h` comes from `instantiate`; the host calls cleanup once, last.
    drop(unsafe { Box::from_raw(h.cast::<Instance>()) });
}

#[cfg(test)]
mod tests {
    use std::ffi::CStr;

    use super::*;

    /// Drives the limiter through the C ABI the way a host does, with in-place buffers.
    #[test]
    #[allow(clippy::cast_possible_truncation)]
    fn host_runs_limiter_in_place() {
        let d = ladspa_descriptor(0);
        assert!(!d.is_null() && ladspa_descriptor(KINDS.len() as c_ulong).is_null());
        let mut l = vec![3.0f32; 1024];
        let mut r = vec![-3.0f32; 1024];
        unsafe {
            let d = &*d;
            assert_eq!(CStr::from_ptr(d.label).to_str(), Ok("limiter"));
            let names: Vec<&str> = (0..d.port_count as usize)
                .map(|i| CStr::from_ptr(*d.port_names.add(i)).to_str().unwrap_or(""))
                .collect();
            assert_eq!(&names[..5], &["In L", "In R", "Out L", "Out R", "Input gain"]);
            let h = (d.instantiate.unwrap_or_else(|| unreachable!()))(d, 48_000);
            let (mut gain, mut ceiling, mut release) = (0.0f32, -6.0f32, 50.0f32);
            let connect = d.connect_port.unwrap_or_else(|| unreachable!());
            for (port, buf) in [(0, l.as_mut_ptr()), (1, r.as_mut_ptr()), (2, l.as_mut_ptr()), (3, r.as_mut_ptr())] {
                connect(h, port, buf);
            }
            connect(h, 4, &raw mut gain);
            connect(h, 5, &raw mut ceiling);
            connect(h, 6, &raw mut release);
            (d.activate.unwrap_or_else(|| unreachable!()))(h);
            (d.run.unwrap_or_else(|| unreachable!()))(h, 1024);
            (d.cleanup.unwrap_or_else(|| unreachable!()))(h);
        }
        let ceil = 10f32.powf(-6.0 / 20.0);
        assert!(l.iter().chain(&r).all(|s| s.abs() <= ceil * 1.0001));
        assert!(l[1000] > ceil * 0.99, "steady state should sit at the ceiling: {}", l[1000]);
    }
}
