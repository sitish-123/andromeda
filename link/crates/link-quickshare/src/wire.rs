//! Generated from `protocol/quickshare/*.proto`; module nesting mirrors the proto packages so cross-package paths resolve.
#![allow(clippy::all, clippy::pedantic, missing_docs)]

pub mod securemessage {
    include!(concat!(env!("OUT_DIR"), "/securemessage.rs"));
}
pub mod securegcm {
    include!(concat!(env!("OUT_DIR"), "/securegcm.rs"));
}
pub mod location {
    pub mod nearby {
        pub mod connections {
            include!(concat!(env!("OUT_DIR"), "/location.nearby.connections.rs"));
        }
        pub mod proto {
            pub mod sharing {
                include!(concat!(env!("OUT_DIR"), "/location.nearby.proto.sharing.rs"));
            }
        }
    }
}
pub mod nearby {
    pub mod sharing {
        pub mod service {
            pub mod proto {
                include!(concat!(env!("OUT_DIR"), "/nearby.sharing.service.proto.rs"));
            }
        }
    }
}

pub use location::nearby::connections;
pub use nearby::sharing::service::proto as sharing;
