//! Agent broker: routes client calls to desktop providers and answers state queries from its cache.

mod client;
pub mod idl;
mod proto;
mod provider;
mod registry;

use std::io;
use std::path::PathBuf;
use std::sync::Arc;

use tokio::net::UnixListener;
use tokio::sync::watch;

pub use proto::MAX_LINE;

pub struct Broker {
    shared: Arc<registry::Shared>,
}

impl Default for Broker {
    fn default() -> Self {
        Self { shared: Arc::new(registry::Shared::new()) }
    }
}

impl Broker {
    /// A counter that grows on each registry or state change.
    pub fn changes(&self) -> watch::Receiver<u64> {
        self.shared.subscribe()
    }

    /// Serves clients on `listener` and links to the provider sockets in `runtime_dir`.
    pub async fn run(self, listener: UnixListener, runtime_dir: PathBuf) -> io::Result<()> {
        tokio::try_join!(
            client::serve(Arc::clone(&self.shared), listener),
            provider::watch(Arc::clone(&self.shared), runtime_dir),
        )
        .map(|_| ())
    }
}
