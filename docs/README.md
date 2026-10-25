# The wobblertc guide

wobblertc gives Jello programs WebRTC peers. These pages explain it from the beginning; read them in order, or go straight to the one you need.

1. [Getting started](getting-started.md): install wobblertc, build its native library, and connect two peers.
2. [Peers](peer-api.md): configuring a peer, offers and answers, candidates, media and data channel messages.
3. [Events](event-model.md): polling a peer, and what each event means.
4. [Signalling](signaling.md): finding the remote peer through WHIP or MQTT.
5. [Codecs](codecs.md): the audio and video codecs, and reading Opus packets.
6. [Metrics](metrics.md): counting what a peer has sent and received.
7. [How it works](architecture.md): the native library, threads, handles, what wobblertc patches in libpeer, and the limits that remain.
8. [Testing](testing.md): how wobblertc is tested, and what needs a real remote peer.

Back to the [README](../README.md).
