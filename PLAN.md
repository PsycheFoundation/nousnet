# observability for a psyche run is fragmented into many parts

a TUI, OTLP tracing, tracing log messages for docker tests with specific structure, a tcp metrics server, logging, iroh metrics.

debugging the system has been attempted through these various channels but it's a bit of a nightmare.

we're overhauling the entire psyche metrics system to use event sourcing.

## the problem

when something goes wrong in this distributed system, we have to answer questions like "node #5 says they sent their training results but node #3 never got them. why did this happen?" or "the witness quorum never formed. which witnesses even saw what data?"

trying to answer these questions right now means we have to scan through noisy logs from multiple systems / multiple hosts, correlating timestamps, and trying to reconstruct what happened from these vague logs and aggregate metrics.

## the solution

record every event required to reconstruct a run as structured data: events with timestamp and context that let us trace a piece of data through the entire system.

this would in theory let us have a global timeline from an entire run that lets us scrub back and forth to view a global state at any point.

we're going to capture:

- every coordinator state change
- every network operation (connection changes, blob transfers, gossip messages)
- every training action (training data downloads, start/complete training)
- model loading / checkpointing
- snapshots of system resources
- attempts to write data to the coordinator

this design would let us switch to a database or something later for actual timeline analysis. the focus is on getting the events correct first.

this ticket doesn't specify how we'll emit events - for now these could just be logs, the ticket focus on what structured data we actually want.

events are facts. they are not derived metrics. we don't need to emit anything that can be derived.

we only track our own actions and observations only we can make. we treat the coordinator as a black box, and only note that we saw it change (when / what it changed to), so that we don't have to spam the entire coordinator struct into logs. (we can reconstruct its state completely from the solana tx history)

## specification

### run context (emitted once at start)

run_started

- run_id
- node_id
- node_role (trainer/witness/etc)
- config
- psyche_version
- start_timestamp

### each subsequent event will include

- **timestamp** - high precision, taken as close to the actual event time as possible
- **coordinator tx id** - the solana transaction id that last changed the coordinator state
- **tags** - vec of things to link related events, e.g. (Blob Upload #378173, Batch ID #183, Blob Hash abc123)

tags expected on events below are denoted in (), unique fields for that event are listed

### specific event types

#### coordinator

saw coordinator state change

- new_state_hash

submitted event (unique req tag)

- type: witness / health check / optimistic warmup etc

event submission complete (unique req tag)

- success/fail
- error_string if fail

#### network and p2p events

p2p connection changed

- endpoint_id
- connection_path (direct vs relayed vs disconnected)

gossip neighbors changed

- removed / new neighbors

p2p peer latency changed

- endpoint_id
- latency

blob added to store (tag for what piece of data (batch # / p2p param / whatever) it is)

- hash

blob upload started (unique tag for this upload, tag for what piece of data it is)

- to_endpoint_id
- size
- retry #

blob upload progress (unique tag) - only emit if >5s since last progress event

- bytes_transferred

blob upload completed (unique tag)

- success/fail
- error_string if fail

blob download started (unique tag for this download, tag for what piece of data it is, tag for its hash)

- from_endpoint_id
- size
- retry #?

blob download progress (unique tag) - only emit if >5s since last progress event

- bytes_transferred

blob download completed (unique tag)

- success/fail
- error_string if fail

gossip message sent (specific data tag, batch id?, blob hash?)

- message_type
- message_size
- \# rebroadcast

new gossip message recv'd (same tags)

- message_type
- message_size

#### training events

batches assignment received (multiple batch ids)

- num_batches

batch data download start (batch id)

- size

batch data download progress (batch id) - only emit if >5s since last progress event

- bytes_downloaded

batch data download complete (batch id)

- success/fail
- error_string if fail

training started (batch id)

training finished (batch id)

distro result deserialize started (batch id)

distro result deserialize completed (batch id)

- success/fail
- error_string if fail

applying distro results (list of batch ids)

#### warmup

p2p param requested from peer

- success/fail
- error_string if fail

checkpoint download started(tag)

- size

checkpoint download progress - only emit if >5s since last progress event (tag)

- bytes downloaded

checkpoint download complete (tag)

- success/fail
- error_string if fail

#### cooldown

model serialization started

model serialization finished

- success/fail
- error_string if fail

checkpoint write started

checkpoint write finished

- success/fail
- error_string if fail

checkpoint upload started

checkpoint upload progress - only emit if >5s since last progress event

checkpoint upload finished

- success/fail
- error_string if fail

#### resource snapshots

emitted on a fixed interval (30s?)

resource snapshot

- gpu_mem_used
- gpu_utilization
- cpu_mem_used
- cpu_utilization
- network_bytes_sent_total
- network_bytes_recv_total
- disk_space_available

# impl stuff:

i think i'm gonna build it events-first
just write to disk or something
new file every time cooldown finishes (some method on the eventstore to mark it as "move on to the next file now please".
)
the eventstore should probably store that like first metadata event so it can keep re-emitting that as the first one at the end of every cooldown,
and also add some run state event to like emit as the seocnd message after every cooldown finishes so we can say "ok starting the next thing now!"
and can have a second job do cleanup depending on the rate it actually writes - so like e.g. we'd only keep the last N rounds
(streaming to disk, written in an append-only way where dying mid-write is readable)
