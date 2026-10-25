# Getting started

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

`build.sh` needs a C compiler (`cc`: Xcode's command-line tools on macOS, clang or gcc on Linux), `patch` and git. The first time, it fetches libpeer and the libraries libpeer uses, at the commit wobblertc pins, into `jdll/third_party/libpeer`, applies wobblertc's fixes to a copy of libpeer, and compiles them; that takes a minute or so. Later builds only compile what changed. The Jello headers come from beside the `jello` on your `PATH`, or from `$JELLO_ROOT/include`. libpeer runs on macOS and Linux; `build.sh` stops with a message on Windows.

## A first connection

A WebRTC connection needs two peers. One makes an *offer*, a session description saying what it can send and where it can be reached; the other takes the offer and makes an *answer*; the first takes the answer. Then both are *polled* until they have connected, and media and messages flow.

Usually the two peers are on different machines, and the descriptions travel through a server (see [Signalling](signaling.md)). This program runs both in one place, each in a process of its own, so that both can be polled at once:

```jello
import std.concurrent.Process as Process;
import type { Process, Inbox } from std.concurrent.Process;
import type { Config } from wobblertc.Peer;
import { Peer, DataChannel, SdpType, defaults } from wobblertc.Peer;
import { Event, State, stateName } from wobblertc.Events;

let config: Config = { ...defaults(), channel: DataChannel.Text };

// The answering peer: it answers the offer it is sent, then echoes messages until it is stopped.
let answerer = fn(offer: Bytes, answerTo: Process<Bytes>, inbox: Inbox<Bytes>) {
  let peer = new Peer(config);
  peer.setRemoteDescription(offer, SdpType.Offer);
  answerTo.send(peer.createAnswer());
  while (inbox.tryReceive() == null) {
    for (event in peer.poll(20)) {
      match (event) {
        Message(stream, data) => { peer.send(data + " back", stream); },
        _ => {},
      }
    }
  }
  peer.close();
};

let offerer = new Peer(config);
let offer = offerer.createOffer();
let answers = new Inbox<Bytes>();
let me = Process.self<Bytes>(answers);
let answering = Process.spawn<Bytes>(fn(inbox: Inbox<Bytes>) { answerer(offer, me, inbox); });
offerer.setRemoteDescription(answers.receive(), SdpType.Answer);

let states = Array.new<Bytes>(0);
let reply = "";
let opened = false;
let started = Process.now();
while (reply == "" && Process.now() - started < 10000) {
  for (event in offerer.poll(20)) {
    match (event) {
      StateChange(state) => { states.push(stateName(state)); },
      Message(_, data) => { reply = data; },
      _ => {},
    }
  }
  if (offerer.state() == State.Completed) {
    if (!opened) { opened = offerer.openChannel("chat"); }
    offerer.send("hello");
  }
}
answering.send("stop");
answering.join();
offerer.close();
`offerer: ${states.join(", ")}\nreply: ${reply}`
```

```text
offerer: checking, connected, completed
reply: hello back
```

This is `examples/loopback.jello`. The pieces:

- `new Peer(config)` makes a peer. `defaults()` is Opus audio, no video, no data channel and no ICE servers; the record spread changes what differs.
- `createOffer` and `createAnswer` return the descriptions, as text; `setRemoteDescription` takes the other side's.
- `poll(20)` runs the peer for up to 20 milliseconds, and returns what happened as `Event`s.
- Once the state is `Completed`, the connection is secure and the data channel can carry messages: `openChannel` opens a stream of it, and `send` sends on it.

The peers connect over this machine's own network address, so it must have one: any interface that is up, not only the loopback one.

## Next

- [Peers](peer-api.md) covers every method.
- [Signalling](signaling.md) connects to a remote peer through a WHIP endpoint or an MQTT broker.
