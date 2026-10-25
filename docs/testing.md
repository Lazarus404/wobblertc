# Testing

```sh
./test.sh
```

`test.sh` builds the native library, then runs the unit tests in `tests/`, and, through degust, the examples in `examples/` and the programs in `tests/programs/`. It needs `jelloc` and `jello` on `PATH`. Arguments before a `--` go to the test binary, such as `-v` or `--run NAME`; those after it go to the tests, such as `--jobs 2`, or `--update` to rewrite the golden files.

No test needs the network or a remote peer. One starts `openssl s_server` on this machine, and is skipped when `openssl` is not on `PATH`.

## What the tests cover

| File | Covers |
|------|--------|
| `tests/wobblertc/Codecs.test.jello` | `opusToc` and `opusFrameMicros` |
| `tests/wobblertc/Events.test.jello` | `stateName` |
| `tests/wobblertc/Metrics.test.jello` | `kbps` |
| `tests/wobblertc/Signaling.test.jello` | `protocol`; signalling again after a failure or `stopSignalling` |
| | a server that never answers the TLS handshake; a local `openssl s_server` whose |
| | self-signed certificate is refused |
| `tests/wobblertc/Peer.test.jello` | decoding events, metrics and ICE servers; offers and answers |
| | refused ICE servers, descriptions, candidates and signalling URLs, with their messages |
| | long description lines; candidates passed over; the 32-candidate limit |
| | sending before connecting; two peers connecting |
| `tests/programs/closed.jello` | a closed peer traps |
| `tests/programs/collected.jello` | the collector frees peers that were not closed |
| `tests/programs/unsendable.jello` | a peer cannot be sent to another process; closing twice |
| `tests/programs/whip_unanswered.jello` | a WHIP endpoint that refuses the connection, twice |
| `examples/offer.jello` | the offer of a peer with audio and video |
| `examples/loopback.jello` | two peers connecting, and a message there and back |

The loopback test, and the example, connect two peers in two processes of one program over the machine's own host address: ICE, the DTLS handshake, SCTP and RTP all run for real, and the test sends a message each way and an audio packet. libpeer offers addresses of the machine's network interfaces, not `127.0.0.1`, so they need an interface with an address, though it need not reach anywhere; the test passes without checking when the offer has no host candidate.

## What needs a real peer

These are not tested, and need something outside the machine:

- WHIP signalling with a WHIP server that answers, such as a media server, and a certificate the machine trusts.
- MQTT signalling with a broker, and a remote peer that offers through it.
- Gathering from STUN and TURN servers.
- Connecting to a browser.
- H.264 video between peers, and key frame requests, which need a decoder on the remote side.
