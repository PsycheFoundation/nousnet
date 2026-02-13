pub mod events;
pub mod store;

pub use events::*;
pub use store::{Backend, EventStore, FileBackend, InMemoryBackend};

mod bytes_visitor;

/// Emit an event to the global event store
#[macro_export]
macro_rules! event {
    // event!(whatever, timestamp, [tag1, tag2])
    ($event:expr, $timestamp:expr, [$($tags:expr),* $(,)?]) => {{
        $crate::EventStore::emit(($event).into(), $timestamp, vec![$($tags),*]);
    }};

    // event!(whatever, [tag1, tag2])
    ($event:expr, [$($tags:expr),* $(,)?]) => {{
        $crate::EventStore::emit(($event).into(), Utc::now(), vec![$($tags),*]);
    }};

    // event!(whatever, timestamp)
    ($event:expr, $timestamp:expr) => {{
        $crate::EventStore::emit(($event).into(), $timestamp, vec![]);
    }};

    // event!(whatever)
    ($event:expr) => {{
        $crate::EventStore::emit(($event).into(), Utc::now(), vec![]);
    }};
}
