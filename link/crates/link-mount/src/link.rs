//! What the mount asks `umbriel-linkd` over D-Bus: the paired phones, their folders, and bytes of their files.

use fuser::Errno;

const SERVICE: &str = "org.umbriel.Link1";
const PATH: &str = "/org/umbriel/Link1";
const INTERFACE: &str = "org.umbriel.Link1";

pub struct Device {
    pub id: String,
    pub name: String,
    pub connected: bool,
}

pub struct Entry {
    pub name: String,
    pub dir: bool,
    pub size: u64,
    pub mtime: u64,
}

#[derive(Clone)]
pub struct Link {
    proxy: zbus::Proxy<'static>,
}

impl Link {
    pub async fn connect() -> zbus::Result<Self> {
        let bus = zbus::Connection::session().await?;
        Ok(Self { proxy: zbus::Proxy::new_owned(bus, SERVICE, PATH, INTERFACE).await? })
    }

    pub async fn devices(&self) -> Result<Vec<Device>, Errno> {
        let devices: Vec<(String, String, bool)> =
            self.proxy.get_property("Devices").await.map_err(|error| errno(&error))?;
        Ok(devices.into_iter().map(|(id, name, connected)| Device { id, name, connected }).collect())
    }

    pub async fn list(&self, device: &str, path: &str) -> Result<Vec<Entry>, Errno> {
        let entries: Vec<(String, bool, u64, u64)> =
            self.proxy.call("ListFiles", &(device, path)).await.map_err(|error| errno(&error))?;
        Ok(entries.into_iter().map(|(name, dir, size, mtime)| Entry { name, dir, size, mtime }).collect())
    }

    pub async fn read(&self, device: &str, path: &str, offset: u64, length: u32) -> Result<Vec<u8>, Errno> {
        self.proxy.call("ReadFile", &(device, path, offset, length)).await.map_err(|error| errno(&error))
    }
}

/// The errno a failed call means to a program reading the mount.
fn errno(error: &zbus::Error) -> Errno {
    let zbus::Error::MethodError(name, message, _) = error else {
        log::warn!("umbriel-linkd: {error}");
        return Errno::EIO;
    };
    match (name.as_str(), message.as_deref()) {
        ("org.umbriel.Link1.Error.Refused", Some("not-found")) | ("org.umbriel.Link1.Error.NotConnected", _) => {
            Errno::ENOENT
        }
        ("org.umbriel.Link1.Error.Refused", Some("not-allowed" | "denied")) => Errno::EACCES,
        ("org.umbriel.Link1.Error.Refused", Some("not-a-folder")) => Errno::ENOTDIR,
        ("org.umbriel.Link1.Error.Refused", Some("not-a-file")) => Errno::EISDIR,
        ("org.umbriel.Link1.Error.Refused", Some("busy")) => Errno::EAGAIN,
        _ => {
            log::info!("umbriel-linkd: {error}");
            Errno::EIO
        }
    }
}
