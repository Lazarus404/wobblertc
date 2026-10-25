# wobblertc

**wobblertc** gives Jello programs WebRTC peers: a program can offer or answer a session, connect to a browser, a media server or another program, and then send and receive encoded audio (Opus, PCMA or PCMU), H.264 video and data channel messages. It can find its remote peer through WHIP or MQTT signalling, or swap descriptions any way you like. Underneath is [libpeer](https://github.com/sepfy/libpeer), a small C WebRTC library for devices, which wobblertc wraps in a `Peer` with Jello methods, enums and events.

**Version:** 0.1.0

```jello
import { Peer, DataChannel, SdpType, defaults } from wobblertc.Peer;
import { Event } from wobblertc.Events;

let peer = new Peer({ ...defaults(), channel: DataChannel.Text });
let offer = peer.createOffer();            // send this to the remote peer...
// ...and set its answer: peer.setRemoteDescription(answer, SdpType.Answer);
for (event in peer.poll(20)) {
  match (event) {
    Message(stream, data) => { peer.send(data, stream); },   // echo every message
    _ => {},
  }
}
peer.close();
```

## Install

Add wobblertc to your project's `jello.json`:

```json
"dependencies": {
  "wobblertc": {"github": "Lazarus404/wobblertc", "tag": "0.1.0"}
}
```

Then fetch it and build its native library:

```sh
jelloc deps.get
packages/wobblertc/build.sh
```

`build.sh` needs a C compiler, `patch` and git: the first time, it fetches libpeer and the libraries it uses (Mbed TLS, libsrtp, usrsctp, cJSON, coreHTTP and coreMQTT) at the commit wobblertc pins, applies wobblertc's fixes to a copy of libpeer, then compiles them all, which takes under a minute. Nothing else need be installed. libpeer runs on macOS and Linux, not on Windows.

`jelloc run` and `jelloc test` find the library in `packages/wobblertc/jdll/` on their own.

## What you get

| Method of `Peer` | Does |
|------------------|------|
| `new Peer(config)` | a peer with a `Config`: codecs, data channel and ICE servers; `defaults()` by default |
| `createOffer`, `createAnswer` | the local description, with the peer's candidates |
| `setRemoteDescription`, `addIceCandidate` | take the remote peer's description and candidates |
| `poll(timeout)` | run the peer; the `Event`s it has to report |
| `state` | where the connection has got to, a `State` |
| `sendAudio`, `sendVideo` | send an encoded audio packet or video frame |
| `openChannel`, `send` | open a data channel stream, and send a message on it |
| `signal`, `stopSignalling` | find the remote peer through WHIP or MQTT |
| `metrics` | what has been sent and received |
| `close` | free the peer now, rather than when it is collected |

`wobblertc.Codecs` also reads the first byte of an Opus packet, and `wobblertc.Signaling` says
which protocol a URL names.

## Documentation

The [guide](docs/README.md) starts at the beginning and covers everything:

| Guide | Covers |
|-------|--------|
| [Getting started](docs/getting-started.md) | Installing, building and a first connection |
| [Peers](docs/peer-api.md) | Configuring a peer, descriptions, candidates, media and messages |
| [Events](docs/event-model.md) | Polling, and what each event means |
| [Signalling](docs/signaling.md) | WHIP and MQTT |
| [Codecs](docs/codecs.md) | The audio and video codecs, and Opus packets |
| [Metrics](docs/metrics.md) | Counting what was sent and received |
| [How it works](docs/architecture.md) | The native library, threads, handles, and what wobblertc patches in libpeer |
| [Testing](docs/testing.md) | Running the tests, and what needs a real peer |

## Development

```sh
./build.sh      # builds jdll/wobblertc.jdll
./test.sh       # builds, then runs the unit tests, the examples and the programs
```

Both need Jello's `jelloc` and `jello` on `PATH`. The examples in `examples/` run as tests, so
they always work as written.

## Licence

The wobblertc library is under the BSD 3-Clause licence. It bundles, in `jdll/third_party/`, libpeer (MIT, by sepfy) and the libraries libpeer vendors, each under its own licence: Mbed TLS (Apache 2.0), libsrtp (BSD 3-Clause, Cisco Systems), usrsctp (BSD 3-Clause), cJSON (MIT), coreHTTP and coreMQTT (MIT, Amazon), and llhttp (MIT). See the licence file in each one's directory. wobblertc's fixes to libpeer, the patches in `jdll/patches/`, are under libpeer's MIT licence.
