My own implementation of fileWatcher.

Feature:
1. Can we increase this by using threads.
2. Can we create as a scalable system.

https://solarianprogrammer.com/2019/01/13/cpp-17-filesystem-write-file-watcher-monitor/

Why (current limits)

Polling-based core: Uses recursive scanning (std::filesystem::recursive_directory_iterator) — costly for large trees and high-frequency changes.
Single-host scope: Designed to watch one local path per process; no built-in network/distributed coordination.
Callback model: Single callback worker thread processes events; may become a bottleneck for high event rates.
State storage: In-memory m_fileMap means no persistence across restarts; no event offsets.
Platform differences: No kernel-level integration (e.g., inotify/ReadDirectoryChanges/FSEvents) — less efficient and potentially inconsistent on different OSes or network filesystems.
Reliability & ordering: No deduplication, batching, ordering guarantees, or exactly-once semantics for downstream consumers.
Security/observability: Lacks auth, encryption, metrics, retries, tracing.

What to change for scalability (single host)

Use event APIs: Replace polling with kernel event watchers (Linux: inotify or fanotify; Windows: ReadDirectoryChangesW; macOS: FSEvents) or a cross-platform library (e.g., libuv, watchman, or a well-maintained wrapper).
Per-directory watches: Watch directories directly (not always entire recursion) to avoid full-tree scans.
Thread pool for callbacks: Use a bounded thread-pool for callback execution and backpressure (e.g., work queue + fixed workers).
Batching and coalescing: Aggregate rapid events into single change notifications to reduce load.
Persistent state: Store file map or last-scan snapshot to disk (or DB) so restarts can resume safely.
Metrics & logs: Add counters, latencies, and health endpoints.

What to change for distributed operation

Agent + central bus pattern: Run a lightweight watcher agent on each host; publish normalized events to a message broker (Kafka, RabbitMQ, NATS). Consumers/processors subscribe centrally.
Idempotency & ordering: Include event IDs, timestamps, checksums; design consumers to be idempotent. Use broker features (partitions, offsets) for ordering and replay.
Leader election for shared mounts: For network filesystems, run one leader agent per mount (use etcd/consul/Zookeeper) to avoid duplicate watches.
Backpressure & batching: Let agents batch events and respect broker rate limits; expose circuit-breakers.
Security: TLS, auth, and ACLs between agents and broker.
Failure handling: Durable event storage on agent, retry/replay logic, and monitoring for lag.
Topology & scaling: Autoscale processors that consume from the broker; shard by directory namespace or tenant.