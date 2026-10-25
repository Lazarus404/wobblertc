# Events

```jello
import { Event, State, stateName } from wobblertc.Events;
```

libpeer does its work when it is run, so a peer must be polled: `poll(timeout)` runs the peer until it has something to report or `timeout` milliseconds have passed, and returns the events since the last poll, oldest first. `poll()`, with no timeout, runs it once and returns at once.

```jello
import { Peer, DataChannel, defaults } from wobblertc.Peer;
import { Event, stateName } from wobblertc.Events;

let peer = new Peer({ ...defaults(), channel: DataChannel.Text });
peer.createOffer();
let seen = Array.new<Bytes>(0);
for (event in peer.poll(20)) {
  match (event) {
    StateChange(state) => { seen.push(`state ${stateName(state)}`); },
    LocalDescription(sdp) => { seen.push(`description of ${sdp.len()} bytes`); },
    Audio(packet) => { seen.push(`audio of ${packet.len()} bytes`); },
    Video(frame) => { seen.push(`video of ${frame.len()} bytes`); },
    ChannelOpen => { seen.push("channel open"); },
    ChannelClose => { seen.push("channel closed"); },
    Message(stream, data) => { seen.push(`message on ${stream}: ${data}`); },
    KeyframeRequest => { seen.push("key frame wanted"); },
    Dropped(count) => { seen.push(`${count} events dropped`); },
  }
}
peer.close();
seen.join("\n")
```

Poll a peer in a loop for as long as it is in use. Media and messages arrive only while it is polled, and a peer that is not polled does not answer its remote peer's checks, which then give up. A poll is a blocking call, so other processes run while one waits.

## The events

| Event | Means |
|-------|-------|
| `StateChange(state)` | the connection moved to `state`; see [Peers](peer-api.md#state) |
| `LocalDescription(sdp)` | the peer's description, with its candidates, which `createOffer` or `createAnswer` also returned |
| `Audio(packet)` | an encoded audio packet from the remote peer |
| `Video(frame)` | an encoded video frame from the remote peer, H.264 NAL units with start codes |
| `ChannelOpen` | the data channel opened: the remote peer opened a stream, or answered `openChannel` |
| `ChannelClose` | the data channel closed |
| `Message(stream, data)` | a message on a stream of the data channel |
| `KeyframeRequest` | the remote peer asked for a key frame, after losing video; send one soon |
| `Dropped(count)` | `count` events were lost because the peer was not polled often enough |

`LocalDescription` is there for programs that make the description in one place and send it in another; most can use what `createOffer` and `createAnswer` return and ignore it. While a peer signals (see [Signalling](signaling.md)), libpeer sends the description itself, and there is no `LocalDescription`.

## Keeping up

A peer keeps up to 4096 events between polls. Past that it drops the oldest, and the next poll starts with `Dropped`, saying how many went. A program receiving audio gets 50 packets a second for 20 millisecond frames, so a poll every few seconds is plenty to lose nothing; a loop that polls with a timeout of 10 to 50 milliseconds, as the examples do, never comes close.

One poll returns at most 4 MiB of events; the rest wait for the next poll.

While the DTLS handshake runs, just after the state becomes `Connected`, libpeer waits for the remote peer's replies inside the poll, so that poll may run some seconds past its timeout.
