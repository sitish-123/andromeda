//! `/org/andromeda/System1/KbdBacklight/<id>`: keyboard backlights. Stage 1 backend: UPower `KbdBacklight`.

use std::collections::HashMap;

use anyhow::{Result, anyhow};
use futures_util::StreamExt;
use tokio::task::JoinHandle;
use zbus::message::Header;
use zbus::object_server::InterfaceRef;
use zbus::{Connection, OwnedObjectPath, interface};

use crate::battery::{UPOWER, UPowerProxy};
use crate::{bus, manager, polkit};

pub const AREA: &str = "keyboard_backlight";
const PATH_PREFIX: &str = "/org/andromeda/System1/KbdBacklight/";

pub fn is_kbd_path(upower_path: &str) -> bool {
    upower_path.starts_with("/org/freedesktop/UPower/KbdBacklight/")
}

#[zbus::proxy(interface = "org.freedesktop.UPower.KbdBacklight", default_service = "org.freedesktop.UPower")]
trait Kbd {
    fn get_brightness(&self) -> zbus::Result<i32>;
    fn get_max_brightness(&self) -> zbus::Result<i32>;
    fn set_brightness(&self, value: i32) -> zbus::Result<()>;
    #[zbus(signal)]
    fn brightness_changed(&self, value: i32) -> zbus::Result<()>;
}

pub struct KbdBacklight {
    upower_path: OwnedObjectPath,
    brightness: i32,
    max_brightness: i32,
}

#[interface(name = "org.andromeda.System1.KbdBacklight")]
impl KbdBacklight {
    #[zbus(property)]
    fn brightness(&self) -> i32 {
        self.brightness
    }
    #[zbus(property)]
    fn max_brightness(&self) -> i32 {
        self.max_brightness
    }

    /// The user's choice, 0 to `MaxBrightness` (clamped).
    async fn set_brightness(
        &self,
        value: i32,
        #[zbus(header)] header: Header<'_>,
        #[zbus(connection)] conn: &Connection,
    ) -> zbus::fdo::Result<()> {
        polkit::check(conn, &header, "org.andromeda.system.keyboard-backlight").await?;
        let kbd = KbdProxy::builder(conn).path(self.upower_path.clone()).build().await?;
        Ok(kbd.set_brightness(value.clamp(0, self.max_brightness)).await?)
    }
}

fn object_path(upower_path: &str) -> String {
    let base = upower_path.rsplit('/').next().unwrap_or_default();
    let id: String = base.chars().map(|c| if c.is_ascii_alphanumeric() { c } else { '_' }).collect();
    format!("{PATH_PREFIX}{}", if id.is_empty() { "_" } else { &id })
}

async fn follow_device(conn: Connection, upower_path: OwnedObjectPath) -> Result<()> {
    let kbd = KbdProxy::builder(&conn).path(upower_path.clone()).build().await?;
    let mut changes = kbd.receive_brightness_changed().await?;
    let object = KbdBacklight {
        upower_path,
        brightness: kbd.get_brightness().await?,
        max_brightness: kbd.get_max_brightness().await?,
    };
    let path = object_path(object.upower_path.as_str());
    conn.object_server().at(path.as_str(), object).await?;
    let iface: InterfaceRef<KbdBacklight> = conn.object_server().interface(path.as_str()).await?;
    while let Some(signal) = changes.next().await {
        let value = *signal.args()?.value();
        if std::mem::replace(&mut iface.get_mut().await.brightness, value) != value {
            iface.get().await.brightness_changed(iface.signal_emitter()).await?;
        }
    }
    Ok(())
}

struct Devices {
    conn: Connection,
    tasks: HashMap<OwnedObjectPath, JoinHandle<()>>,
}

impl Devices {
    async fn sync(&mut self, upower: &UPowerProxy<'_>) -> Result<()> {
        let paths = upower.enumerate_kbd_backlights().await?;
        let gone: Vec<_> = self.tasks.keys().filter(|p| !paths.contains(p)).cloned().collect();
        for path in gone {
            self.remove(&path).await;
        }
        for path in paths {
            if self.tasks.contains_key(&path) {
                continue;
            }
            let conn = self.conn.clone();
            let key = path.clone();
            let task = tokio::spawn(async move {
                if let Err(e) = follow_device(conn.clone(), path.clone()).await {
                    log::warn!("keyboard backlight {path}: {e:#}");
                }
                let _ = conn.object_server().remove::<KbdBacklight, _>(object_path(path.as_str())).await;
            });
            self.tasks.insert(key, task);
        }
        Ok(())
    }

    async fn remove(&mut self, path: &OwnedObjectPath) {
        if let Some(task) = self.tasks.remove(path) {
            task.abort();
        }
        let _ = self.conn.object_server().remove::<KbdBacklight, _>(object_path(path.as_str())).await;
    }

    async fn clear(&mut self) {
        let paths: Vec<_> = self.tasks.keys().cloned().collect();
        for path in &paths {
            self.remove(path).await;
        }
    }
}

pub async fn run(conn: Connection) {
    let devices = &tokio::sync::Mutex::new(Devices { conn: conn.clone(), tasks: HashMap::new() });
    let conn = &conn;
    bus::supervise(
        conn,
        UPOWER,
        AREA,
        move || async move { follow(conn, &mut *devices.lock().await).await },
        move || async move {
            devices.lock().await.clear().await;
            Ok(())
        },
    )
    .await;
}

async fn follow(conn: &Connection, devices: &mut Devices) -> Result<()> {
    let gone = bus::vanished(conn, UPOWER);
    let upower = UPowerProxy::new(conn).await?;
    let mut added = upower.receive_device_added().await?;
    let mut removed = upower.receive_device_removed().await?;
    devices.sync(&upower).await?;
    manager::set_area(conn, AREA, true, "").await?;
    tokio::pin!(gone);
    loop {
        let changed = tokio::select! {
            result = &mut gone => return result,
            Some(signal) = added.next() => signal.args()?.device().clone(),
            Some(signal) = removed.next() => signal.args()?.device().clone(),
            else => return Err(anyhow!("signal streams ended")),
        };
        if is_kbd_path(changed.as_str()) {
            devices.sync(&upower).await?;
        }
    }
}
