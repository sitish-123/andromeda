//! Helpers to follow a backend service: its properties as a map, and its owner coming and going.

use std::collections::HashMap;

use anyhow::{Context, Result};
use futures_util::StreamExt;
use zbus::fdo::{DBusProxy, PropertiesProxy};
use zbus::names::{BusName, InterfaceName};
use zbus::{Connection, OwnedVariant};

pub type Props = HashMap<String, OwnedVariant>;

pub fn get<T: TryFrom<OwnedVariant>>(props: &Props, key: &str) -> Option<T> {
    props.get(key).and_then(|v| v.try_clone().ok()).and_then(|v| T::try_from(v).ok())
}

/// The properties of one interface of a remote object, kept current from `PropertiesChanged`.
pub struct Remote {
    proxy: PropertiesProxy<'static>,
    interface: InterfaceName<'static>,
    changes: zbus::fdo::PropertiesChangedStream,
    pub props: Props,
}

impl Remote {
    /// Subscribes before the first `GetAll`, so no change is lost between the two. The call activates the service.
    pub async fn new(conn: &Connection, service: &'static str, path: String, interface: &'static str) -> Result<Self> {
        let proxy = PropertiesProxy::builder(conn).destination(service).path(path).build().await?;
        let changes = proxy.receive_properties_changed().await?;
        let interface = InterfaceName::from_static_str(interface)?;
        let props = proxy.get_all(interface.clone()).await.with_context(|| format!("{service}: GetAll"))?;
        Ok(Self { proxy, interface, changes, props })
    }

    /// Waits for the next change of this interface and merges it. `None` when the stream ends.
    pub async fn next(&mut self) -> Option<Result<()>> {
        loop {
            let signal = self.changes.next().await?;
            let Ok(args) = signal.args() else { continue };
            if args.interface_name() != &self.interface {
                continue;
            }
            for (key, value) in args.changed_properties() {
                match OwnedVariant::try_from(value) {
                    Ok(v) => {
                        self.props.insert((*key).to_owned(), v);
                    }
                    Err(e) => return Some(Err(e.into())),
                }
            }
            for key in args.invalidated_properties().iter() {
                match self.proxy.get(self.interface.clone(), key).await {
                    Ok(v) => {
                        self.props.insert((*key).to_owned(), v);
                    }
                    Err(e) => return Some(Err(e.into())),
                }
            }
            return Some(Ok(()));
        }
    }
}

/// Follows one backend service for the life of the daemon: `follow` runs while the service is up and returns when it
/// stops or fails; then `lost` clears the area's state, the area is marked unavailable with the reason, and the loop
/// waits for the service to start again (a restart or a later install).
pub async fn supervise<F, FF, L, LF>(conn: &Connection, service: &'static str, area: &str, mut follow: F, mut lost: L)
where
    F: FnMut() -> FF,
    FF: Future<Output = Result<()>>,
    L: FnMut() -> LF,
    LF: Future<Output = zbus::Result<()>>,
{
    loop {
        let reason = match follow().await {
            Ok(()) => format!("{service} stopped"),
            Err(e) => format!("{service}: {e:#}"),
        };
        log::info!("{area}: {reason}");
        if let Err(e) = lost().await.and(crate::manager::set_area(conn, area, false, &reason).await) {
            log::warn!("{area}: publishing the loss failed: {e}");
        }
        // A failure while the service still runs waits for its restart, so a broken backend cannot spin this loop.
        if let Err(e) = async {
            vanished(conn, service).await?;
            appeared(conn, service).await
        }
        .await
        {
            log::warn!("{area}: waiting for {service} failed: {e}");
            return;
        }
    }
}

/// Resolves when `name` loses its owner.
pub async fn vanished(conn: &Connection, name: &str) -> Result<()> {
    let dbus = DBusProxy::new(conn).await?;
    let mut changes = dbus.receive_name_owner_changed_with_args(&[(0, name)]).await?;
    if !dbus.name_has_owner(BusName::try_from(name)?).await? {
        return Ok(());
    }
    while let Some(signal) = changes.next().await {
        if signal.args()?.new_owner().is_none() {
            return Ok(());
        }
    }
    Ok(())
}

/// Resolves when `name` gets an owner (a restart, or a later install).
pub async fn appeared(conn: &Connection, name: &str) -> Result<()> {
    let dbus = DBusProxy::new(conn).await?;
    let mut changes = dbus.receive_name_owner_changed_with_args(&[(0, name)]).await?;
    if dbus.name_has_owner(BusName::try_from(name)?).await? {
        return Ok(());
    }
    while let Some(signal) = changes.next().await {
        if signal.args()?.new_owner().is_some() {
            return Ok(());
        }
    }
    Ok(())
}
