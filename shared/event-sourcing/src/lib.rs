pub mod coordinator_source;
pub mod events;
pub mod projection;
pub mod store;
pub mod timeline;

pub use events::*;
pub use store::{Backend, EventStore, FileBackend, InMemoryBackend};

mod bytes_visitor;

pub use chrono::Utc;

/// Emit an event to the global event store
#[macro_export]
macro_rules! event {
    // event!(whatever, timestamp, [tag1, tag2])
    ($event:expr, $timestamp:expr, [$($tags:expr),* $(,)?]) => {{
        #[allow(unused_imports)]
        use $crate::events::*;
        $crate::EventStore::emit(($event).into(), $timestamp, vec![$($tags),*]);
    }};

    // event!(whatever, [tag1, tag2])
    ($event:expr, [$($tags:expr),* $(,)?]) => {{
        #[allow(unused_imports)]
        use $crate::events::*;
        $crate::EventStore::emit(($event).into(), $crate::Utc::now(), vec![$($tags),*]);
    }};

    // event!(whatever, timestamp)
    ($event:expr, $timestamp:expr) => {{
        #[allow(unused_imports)]
        use $crate::events::*;
        $crate::EventStore::emit(($event).into(), $timestamp, vec![]);
    }};

    // event!(whatever)
    ($event:expr) => {{
        #[allow(unused_imports)]
        use $crate::events::*;
        $crate::EventStore::emit(($event).into(), $crate::Utc::now(), vec![]);
    }};
}
