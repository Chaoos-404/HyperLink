# HyperLink Architecture

HyperLink is currently a Node.js foundation implementation of the Project BoltBridge draft. The code is organized so the transport can later be replaced with QUIC or a native Rust core without changing the CLI or GUI surface.

## Implemented

- CLI commands for `serve`, `send`, `discover`, `interfaces`, and `gui`.
- DNS-SD style multicast discovery using `_boltbridge._tcp.local`.
- Link-local interface detection for `169.254.0.0/16` and `fe80::/10`.
- Streaming directory traversal. Files are sent as they are discovered rather than through a full in-memory manifest.
- Bounded block streaming with negotiated `xxh64` block hashes and full-file hashes. BLAKE2b remains available as a compatibility fallback for older peers.
- Receiver-side disk-space checks before each file.
- Block acknowledgements and retry on failed block verification.
- Safe receive paths that reject traversal outside the destination folder.
- A local browser GUI that can discover peers and drag/drop files to a selected receiver.
- Send/receive diagnostics for identifying whether large transfer time is dominated by hashing, socket backpressure, acknowledgement latency, or receiver disk writes.

## Intentional Gaps

- The current transport uses TCP with per-block acknowledgements. The PRD target calls for QUIC or multiplexed parallel TCP streams; that belongs in a dedicated transport module.
- The Node core now uses WebAssembly XXHash64 for the hot integrity path. A native core should still move this to BLAKE3 or XXHash64 with SIMD acceleration and tighter memory control.
- Resume after cable disconnect is not complete. The protocol already has stable file paths, block indexes, and hashes, so resumable manifests can be added without changing the frame envelope.
- The browser GUI cannot access arbitrary local folder paths directly. It streams dropped files through the local HTTP server to the receiver.
