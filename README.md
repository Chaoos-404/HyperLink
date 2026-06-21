# HyperLink

HyperLink is a foundation package for high-speed transfer between devices. It currently provides a peer-to-peer file transfer utility for direct wired links such as Thunderbolt Bridge, USB4 networking, and other link-local interfaces.

## Quick Start

Install dependencies:

```sh
npm install
```

Start a receiver:

```sh
npm start -- serve --dest received
```

Send files from another terminal or machine:

```sh
npm start -- send ./some-file --host 169.254.x.x
```

Find peers and interfaces:

```sh
npm start -- discover
npm start -- interfaces
```

Open the lightweight GUI:

```sh
npm start -- gui
```

## Notes

- Prefer link-local addresses (`169.254.0.0/16` or `fe80::/10`) for direct wired transfers.
- Transfers are streamed in bounded blocks and verified block-by-block.
- Current peers negotiate `xxh64` integrity checks when both sides are up to date, with BLAKE2b fallback for older receivers.
- Current peers also negotiate pipelined block acknowledgements; older receivers automatically fall back to stop-and-wait mode.
- Use `--diagnose` on `send` or `serve` to print timing counters for hash, socket, acknowledgement, and disk-write phases.
- Use `--token secret` on both `serve` and `send` when operating on shared networks.
- See `docs/ARCHITECTURE.md` for the implemented scope and remaining PRD gaps.
