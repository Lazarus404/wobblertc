# Metrics

```jello
import type { Metrics } from wobblertc.Metrics;
import { kbps } from wobblertc.Metrics;
```

`metrics()` says what a peer has sent and received since it was made, as a `Metrics` record of `I64`s:

| Field | Counts |
|-------|--------|
| `audioBytesSent`, `audioPacketsSent` | audio packets `sendAudio` sent, and their bytes |
| `audioBytesReceived`, `audioPacketsReceived` | audio packets received from the remote peer |
| `videoBytesSent`, `videoFramesSent` | video frames `sendVideo` sent |
| `videoBytesReceived`, `videoFramesReceived` | video frames received |
| `messagesSent`, `messagesReceived` | data channel messages |

Bytes are of the encoded media or messages, not of the packets on the network. A send that returned `false` counts nothing.

`kbps(bytes, millis)` turns a count of bytes over a time into kilobits a second, and is 0 when no time has passed:

```jello
import { Peer } from wobblertc.Peer;
import { kbps } from wobblertc.Metrics;

let peer = new Peer();
let before = peer.metrics();
// ... send for a second ...
let after = peer.metrics();
peer.close();
kbps(after.audioBytesSent - before.audioBytesSent, 1000i64)
```

libpeer does not yet read the reports of loss that RTCP carries, so there are no figures for packets lost.
