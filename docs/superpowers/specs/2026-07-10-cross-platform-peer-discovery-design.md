# Cross-Platform Peer Discovery and Connection Design

## Goal

Provide a reusable C++20 API that discovers Hyperlink peers on directly connected or local IPv4 networks and connects to the peer with the highest measured TCP throughput. The implementation shall support macOS, Linux, and Windows without third-party runtime dependencies.

## Scope

The feature shall discover only peers on locally attached IPv4 subnets, including USB or Thunderbolt link-local `169.254.0.0/16` networks. It shall not use Internet services, multicast DNS, relays, or cross-subnet routing.

The existing manual TCP endpoint flow remains available. The `hyperlink send --auto` command will use the new discovery API; `--host` continues to bypass discovery.

## Architecture

The library will add a peer-discovery component, separate from the file-transfer example. It owns UDP discovery requests, cross-platform IPv4 interface enumeration, response parsing, candidate de-duplication, and throughput ranking. It exposes candidates and the selected peer to applications without exposing platform socket handles.

The component has three replaceable internal boundaries:

- An IPv4 interface provider returns active interface addresses and their subnet broadcast addresses.
- A UDP discovery transport sends one request per broadcast address and receives replies until the discovery deadline.
- A TCP probe runner measures an advertised probe endpoint and returns either a throughput result or a concrete failure reason.

Production code uses native sockets. Unit tests inject deterministic interface, UDP, and TCP-probe implementations so they do not require a physical network.

## Public API

`include/hyperlink/peer_discovery.hpp` will define the following value types:

- `PeerEndpoint`: IPv4 host, transfer port, probe port, and parallel-stream count.
- `DiscoveredPeer`: endpoint, advertised display name, measured bytes per second, and discovery source address.
- `PeerDiscoveryOptions`: UDP discovery port (default `47789`), discovery timeout (default `1500 ms`), per-candidate TCP probe timeout (default `500 ms`), and probe payload size (default `256 KiB`).
- `PeerDiscoveryError`: an exception with a message that distinguishes no UDP replies from replies that failed all TCP probes.

`PeerDiscovery::discover(options)` returns all syntactically valid, de-duplicated responders. `PeerDiscovery::select_fastest(options)` probes each discovered endpoint and returns the peer with the largest measured throughput. Ties are resolved deterministically by ascending IPv4 address and then ascending transfer port.

`PeerDiscovery::connect_fastest(options, TcpEndpointOverrides)` returns a `TcpTransport` connected to the selected transfer endpoint. Buffer-size overrides supplied by the caller are preserved; the selected host and port replace only the corresponding endpoint fields.

## Discovery Protocol

Discovery uses IPv4 UDP broadcast only. The requester sends the ASCII request below to every active interface broadcast address and to loopback for local testing:

```text
HLINK_DISCOVER_V2
```

An advertising receiver responds unicast to the request source with:

```text
HLINK_PEER_V2 <transfer-port> <probe-port> <parallel-streams> <display-name>
```

All numeric fields are unsigned decimal values. Transfer and probe ports must be in `1..65535`; parallel streams must be positive. A response longer than 512 bytes, with an invalid prefix, or with invalid numeric fields is ignored. The response source IPv4 address is authoritative; a peer cannot nominate another address in its payload.

The receiver runs a lightweight TCP probe listener at the advertised probe port. On each accepted connection it writes a fixed byte sequence until the remote end closes or the per-connection probe window expires. It accepts no application data and does not authenticate the probe; therefore the listener is restricted to the same network exposure as the existing file-transfer receiver and is started only while discovery advertising is enabled.

## Platform Behavior

On macOS and Linux, IPv4 interfaces are enumerated through `getifaddrs`. Only `IFF_UP` interfaces with an IPv4 address participate. The directed broadcast address comes from `ifa_broadaddr` when present; otherwise it is calculated from the interface netmask.

On Windows, interfaces are enumerated through `GetAdaptersAddresses(AF_INET)`. Only adapters whose operational status is up participate. The directed broadcast address is calculated from each unicast address and its `OnLinkPrefixLength`.

Each platform also tries the limited broadcast address `255.255.255.255` for every active local binding. Duplicate `(local-address, broadcast-address)` targets are removed before sending. Socket startup and cleanup are encapsulated so Winsock initialization is transparent to callers.

## Candidate Selection and Connection

After the UDP deadline, responders are de-duplicated by their source IPv4 address, transfer port, and probe port. Each candidate is probed independently using a TCP connection to its advertised probe port.

For a successful probe, the client reads data for at most `probe_timeout` and computes bytes read divided by elapsed monotonic time. A candidate that cannot connect, closes before receiving data, times out, or reports a socket error is excluded and recorded with its failure reason. The fastest remaining candidate wins. If every candidate fails, selection throws an error listing the candidate addresses and failures.

The selected peer is then passed to the existing TCP transport connection path. Discovery does not change file-transfer framing, session negotiation, or the manual endpoint API.

## Error Handling

- No usable local interfaces: return an error that discovery could not create any broadcast target.
- Socket creation or binding failure for one interface: skip that target and continue with remaining targets.
- No valid replies before the deadline: return `no Hyperlink peer discovered`.
- Invalid reply: ignore it without terminating discovery.
- All probes fail: return `no discovered peer accepted a TCP throughput probe`, followed by per-peer reasons.
- A successful probe followed by transfer connection failure: surface the existing TCP transport error; the implementation does not retry a slower peer automatically.

## Testing

Unit tests shall cover response parsing, numeric range validation, interface-target de-duplication, and deterministic tie ordering. Probe-selection tests shall use a fake probe runner to cover highest-throughput selection, connection failure, zero-byte response, timeout, and the all-failed diagnostic.

An integration-style loopback test shall start the production responder and probe listener, discover it through `127.0.0.1`, measure a positive throughput result, and connect to the advertised transfer endpoint. The test will be skipped only when the platform cannot bind a loopback UDP socket; normal macOS, Linux, and Windows builds must execute it.

## Non-Goals

- IPv6 discovery or connection selection.
- Cross-subnet, Internet, relay, or multicast-DNS discovery.
- Persistent peer registries or background discovery daemons.
- Encryption or authentication changes to the existing transfer protocol.
- Automatic retry against the second-fastest peer after transfer connection failure.
