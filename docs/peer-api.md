# Peers

```jello
import type { Config, IceServer } from wobblertc.Peer;
import { Peer, DataChannel, SdpType, defaults, maxAudioPacket, maxIceServers } from wobblertc.Peer;
```

A `Peer` is one end of a WebRTC connection. This page covers making one, the offer and answer, candidates, sending media and messages, and closing it. What a peer reports back, through `poll`, is on the [Events](event-model.md) page.

## Configuring a peer

A peer's configuration is a `Config` record, fixed when the peer is made:

| Field | Type | Says |
|-------|------|------|
| `audio` | `AudioCodec` | the audio codec: `Opus`, `Pcma`, `Pcmu` or `None` |
| `video` | `VideoCodec` | the video codec: `H264` or `None` |
| `channel` | `DataChannel` | whether there is a data channel: `Text`, `Binary` or `None` |
| `iceServers` | `Array<IceServer>` | STUN and TURN servers to gather candidates from |

`defaults()` gives Opus audio, no video, no data channel and no ICE servers, and `new Peer()` uses it. To change some fields, spread the defaults into a new record:

```jello
import { Peer, DataChannel, defaults } from wobblertc.Peer;
import { AudioCodec, VideoCodec } from wobblertc.Codecs;

let phone = new Peer({ ...defaults(), audio: AudioCodec.Pcma });
let camera = new Peer({ ...defaults(), video: VideoCodec.H264, channel: DataChannel.Binary });
phone.close();
camera.close();
```

A `Text` data channel marks its messages as text, and a `Binary` one as binary; either carries any bytes.

### ICE servers

A peer always offers its own host address. To be reachable from beyond its network, it also asks STUN servers what its address looks like from outside, and TURN servers to relay for it. An `IceServer` is a URL, `stun:HOST:PORT` or `turn:HOST:PORT`, and for TURN a username and credential (empty for STUN):

```jello
import { Peer, defaults } from wobblertc.Peer;

let peer = new Peer({ ...defaults(), iceServers: [
  { url: "stun:stun.l.google.com:19302", username: "", credential: "" },
] });
peer.close();
```

A peer takes at most `maxIceServers`, 5. A URL of another scheme, or without a host and port, throws.

Gathering asks each server over the network, so with ICE servers, `createOffer` and `createAnswer` wait for their replies, for up to a few seconds. They are blocking calls: the rest of the program carries on meanwhile.

## The offer and the answer

| Method | Does |
|--------|------|
| `createOffer(): Bytes` | an offer: the peer's description, with its candidates |
| `createAnswer(): Bytes` | an answer to the remote offer, with the peer's candidates |
| `setRemoteDescription(sdp, kind: SdpType): Peer` | takes the remote peer's offer or answer |
| `addIceCandidate(candidate): Peer` | takes one more candidate of the remote peer |

The peer that offers calls `createOffer`, sends the offer to the remote peer, and gives the answer that comes back to `setRemoteDescription(answer, SdpType.Answer)`. The peer that answers calls `setRemoteDescription(offer, SdpType.Offer)` and then `createAnswer`. Taking an answer, or making one, starts the checks of candidate pairs, and the peer's state becomes `Checking`.

A description is Session Description Protocol text. Its lines may end in `"\r\n"` or `"\n"`. libpeer gathers all its candidates before it returns a description, so the description already holds them: there is no trickle of candidates afterwards.

A description may have lines of any length. `setRemoteDescription` throws, changing nothing, when the description:

- is empty, or holds a zero byte;
- has a malformed candidate or fingerprint;
- has a candidate field, ICE username fragment or password, or fingerprint longer than libpeer keeps;
- would give the peer more remote candidates than libpeer keeps, 32 since the last offer.

Candidates libpeer cannot use, such as TCP ones, are passed over.

`addIceCandidate` takes a candidate line, as `candidate:...` or `a=candidate:...`, for a remote peer that sends its candidates separately; an empty one, which ends the candidates, adds nothing. Only UDP candidates are used: a candidate libpeer cannot read or use throws, as does a 33rd.

## Sending

| Method | Does |
|--------|------|
| `sendAudio(packet): Bool` | sends an encoded audio packet |
| `sendVideo(frame): Bool` | sends an encoded video frame |
| `openChannel(label, stream = 0): Bool` | asks the remote peer to open a data channel stream |
| `send(data, stream = 0): Bool` | sends a message on a stream of the data channel |

Each returns whether it went, and nothing goes until the connection is ready: media once the state is `Completed`, messages once the data channel is open as well. Before then they return `false`, so a program can send from the start without checking.

`sendAudio` takes one encoded packet, such as an Opus frame or 20 milliseconds of G.711, of at most `maxAudioPacket`, 1200, bytes; `sendVideo` takes H.264 NAL units with start codes, which it splits into RTP packets. See [Codecs](codecs.md). Sending audio from a peer without an audio codec throws, and likewise video, and messages without a data channel.

The peer that offers usually opens the data channel, once the state is `Completed`; the remote peer then reports `Event.ChannelOpen`, and messages can flow both ways. A stream is a number from 0 to 65535; messages on stream 0 need no `openChannel` from a peer whose remote peer opened it.

## State

`state()` says where the connection has got to, as a `State`:

| State | Means |
|-------|-------|
| `New` | not yet checking: no answer has been made or taken |
| `Checking` | checking candidate pairs |
| `Connected` | a pair answered; the DTLS handshake is under way |
| `Completed` | DTLS is done: media and messages can flow |
| `Failed` | no candidate pair answered |
| `Disconnected` | not used by libpeer at present |
| `Closed` | the remote peer stopped answering, or a signalling server closed the peer |

Each change is also reported by `poll`, as `Event.StateChange`. `stateName` in `wobblertc.Events` gives a state's name, such as `"checking"`.

## Closing

`close()` closes the peer at once: it stops its signalling, closes its sockets and frees it. Using a closed peer is a bug, and stops the program with the trap `TYPE_MISMATCH`; closing it twice does nothing. A peer that is not closed is freed when the garbage collector finds it unreachable.

A peer stays in the process that made it: sending one to another process stops the program with the trap `UNSENDABLE`. To run two peers at once, make each in a process of its own, as [Getting started](getting-started.md) does.
