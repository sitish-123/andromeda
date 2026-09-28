//! The window: the phone's screen, letterboxed, and every tap, drag, scroll, and key sent back as `MirrorInput` in
//! the phone's coordinates (1/10000 of the video's width and height).

use std::cell::Cell;
use std::rc::Rc;
use std::time::Instant;

use gstreamer as gst;
use gstreamer::prelude::*;
use gtk4::prelude::*;
use gtk4::{gdk, gio, glib};

use crate::stream::Display;
use crate::{LINK, LINK_PATH};

const UNIT: f64 = 10_000.0;
/// A press that moves less than this is a tap, or a long press when held past [`LONG_PRESS_MS`].
const TAP_SLOP_PX: f64 = 12.0;
const LONG_PRESS_MS: u128 = 500;
/// One wheel notch scrolls this fraction of the screen.
const SCROLL_STEP: f64 = 0.15;

/// The video's rectangle inside the picture, which keeps the phone's aspect.
#[derive(Clone, Copy)]
struct Frame {
    width: f64,
    height: f64,
}

impl Frame {
    /// A widget point in phone units, clamped to the screen's edge.
    fn to_phone(self, widget: &impl IsA<gtk4::Widget>, x: f64, y: f64) -> (i32, i32) {
        let (area_w, area_h) = (f64::from(widget.width()), f64::from(widget.height()));
        let scale = (area_w / self.width).min(area_h / self.height);
        let (shown_w, shown_h) = (self.width * scale, self.height * scale);
        let fx = ((x - (area_w - shown_w) / 2.0) / shown_w).clamp(0.0, 1.0);
        let fy = ((y - (area_h - shown_h) / 2.0) / shown_h).clamp(0.0, 1.0);
        #[expect(clippy::cast_possible_truncation, reason = "clamped to 0..=10000")]
        ((fx * UNIT).round() as i32, (fy * UNIT).round() as i32)
    }
}

pub fn window(
    app: &gtk4::Application,
    bus: &gio::DBusConnection,
    device: &str,
    name: &str,
    display: &Rc<Display>,
    width: u32,
    height: u32,
) {
    let frame = Frame { width: f64::from(width), height: f64::from(height) };
    let picture = gtk4::Picture::for_paintable(&display.paintable);
    picture.set_content_fit(gtk4::ContentFit::Contain);
    picture.set_can_focus(true);
    picture.set_focusable(true);
    let send = sender(bus, device);

    let header = gtk4::HeaderBar::new();
    for (icon, tooltip, action) in [
        ("go-previous-symbolic", "Back", "back"),
        ("go-home-symbolic", "Home", "home"),
        ("view-app-grid-symbolic", "Recent apps", "recents"),
    ] {
        let button = gtk4::Button::from_icon_name(icon);
        button.set_tooltip_text(Some(tooltip));
        let send = send.clone();
        button.connect_clicked(move |_| send(action, glib::VariantDict::new(None)));
        header.pack_start(&button);
    }

    add_pointer(&picture, frame, &send);

    let keys = gtk4::EventControllerKey::new();
    {
        let send = send.clone();
        keys.connect_key_pressed(move |_, key, _, _| {
            let text = match key {
                gdk::Key::Escape => return send_now(&send, "back", None),
                gdk::Key::BackSpace => "\u{8}".to_owned(),
                gdk::Key::Return | gdk::Key::KP_Enter => "\n".to_owned(),
                other => match other.to_unicode().filter(|c| !c.is_control()) {
                    Some(c) => c.to_string(),
                    None => return glib::Propagation::Proceed,
                },
            };
            send_now(&send, "text", Some(text))
        });
    }

    let window = gtk4::ApplicationWindow::builder()
        .application(app)
        .title(name)
        .child(&picture)
        .default_width(i32::try_from(width / 2).unwrap_or(540))
        .default_height(i32::try_from(height / 2).unwrap_or(1200))
        .build();
    window.set_titlebar(Some(&header));
    window.add_controller(keys);
    {
        let display = display.clone();
        window.connect_close_request(move |_| {
            display.stop();
            glib::Propagation::Proceed
        });
    }
    close_when_ended(&window, display);
    window.present();
    picture.grab_focus();
}

/// Press and release as a tap, long press, or swipe; the wheel as a scroll at the pointer.
fn add_pointer(picture: &gtk4::Picture, frame: Frame, send: &Send) {
    let picture = picture.clone();
    let pressed = Rc::new(Cell::new(None::<Instant>));
    let drag = gtk4::GestureDrag::new();
    {
        let pressed = pressed.clone();
        drag.connect_drag_begin(move |_, _, _| pressed.set(Some(Instant::now())));
    }
    {
        let (send, picture) = (send.clone(), picture.clone());
        drag.connect_drag_end(move |gesture, dx, dy| {
            let Some((sx, sy)) = gesture.start_point() else { return };
            let held = pressed.take().map_or(0, |at| at.elapsed().as_millis());
            let (x, y) = frame.to_phone(&picture, sx, sy);
            let args = glib::VariantDict::new(None);
            args.insert("x", x);
            args.insert("y", y);
            if dx.hypot(dy) < TAP_SLOP_PX {
                send(if held >= LONG_PRESS_MS { "long-press" } else { "tap" }, args);
            } else {
                let (x2, y2) = frame.to_phone(&picture, sx + dx, sy + dy);
                args.insert("x2", x2);
                args.insert("y2", y2);
                args.insert("ms", u32::try_from(held.clamp(50, 2000)).unwrap_or(300));
                send("swipe", args);
            }
        });
    }
    picture.add_controller(drag);

    let pointer = Rc::new(Cell::new((0.0, 0.0)));
    let motion = gtk4::EventControllerMotion::new();
    {
        let pointer = pointer.clone();
        motion.connect_motion(move |_, x, y| pointer.set((x, y)));
    }
    picture.add_controller(motion);
    let scroll = gtk4::EventControllerScroll::new(gtk4::EventControllerScrollFlags::BOTH_AXES);
    {
        let (send, picture) = (send.clone(), picture.clone());
        scroll.connect_scroll(move |_, dx, dy| {
            let (px, py) = pointer.get();
            let (x, y) = frame.to_phone(&picture, px, py);
            let args = glib::VariantDict::new(None);
            args.insert("x", x);
            args.insert("y", y);
            // Content follows the fingers: a wheel turned down moves a finger up.
            #[expect(clippy::cast_possible_truncation, reason = "clamped to -10000..=10000")]
            let delta = |d: f64| (-d * SCROLL_STEP * UNIT).clamp(-UNIT, UNIT).round() as i32;
            args.insert("x2", delta(dx));
            args.insert("y2", delta(dy));
            send("scroll", args);
            glib::Propagation::Stop
        });
    }
    picture.add_controller(scroll);
}

type Send = Rc<dyn Fn(&str, glib::VariantDict)>;

fn send_now(send: &Send, action: &str, text: Option<String>) -> glib::Propagation {
    let args = glib::VariantDict::new(None);
    if let Some(text) = text {
        args.insert("text", text);
    }
    send(action, args);
    glib::Propagation::Stop
}

fn sender(bus: &gio::DBusConnection, device: &str) -> Send {
    let (bus, device) = (bus.clone(), device.to_owned());
    Rc::new(move |action: &str, args: glib::VariantDict| {
        let params = (device.as_str(), action, args.end()).to_variant();
        bus.call(
            Some(LINK),
            LINK_PATH,
            LINK,
            "MirrorInput",
            Some(&params),
            None,
            gio::DBusCallFlags::NONE,
            5_000,
            None::<&gio::Cancellable>,
            |reply| {
                if let Err(error) = reply {
                    log::info!("mirror input: {error}");
                }
            },
        );
    })
}

/// Asks for a keyframe, so the picture starts at once even if the phone's first one was lost.
pub fn keyframe(bus: &gio::DBusConnection, device: &str) {
    bus.call(
        Some(LINK),
        LINK_PATH,
        LINK,
        "MirrorKeyframe",
        Some(&(device,).to_variant()),
        None,
        gio::DBusCallFlags::NONE,
        5_000,
        None::<&gio::Cancellable>,
        |reply| {
            if let Err(error) = reply {
                log::debug!("asking for a keyframe: {error}");
            }
        },
    );
}

/// The phone stopping ends the stream; the window closes with it.
fn close_when_ended(window: &gtk4::ApplicationWindow, display: &Rc<Display>) {
    let Some(bus) = display.pipeline.bus() else { return };
    let window = window.downgrade();
    let watch = bus.add_watch_local(move |_, message| {
        let ended = match message.view() {
            gst::MessageView::Eos(_) => true,
            gst::MessageView::Error(error) => {
                log::warn!("mirroring: {}", error.error());
                true
            }
            _ => false,
        };
        if ended && let Some(window) = window.upgrade() {
            window.close();
        }
        glib::ControlFlow::Continue
    });
    // The guard would remove the watch when dropped; the window's life bounds it instead.
    if let Ok(watch) = watch {
        std::mem::forget(watch);
    }
}
