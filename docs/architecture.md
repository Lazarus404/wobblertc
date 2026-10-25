# How it works

This page is for anyone changing wobblertc, or wondering what happens under a `Peer`: the layers, how events cross from libpeer to Jello, how a peer is freed, what wobblertc patches in libpeer, and the limits that remain.

## The layers

| Layer | Where | Does |
|-------|-------|------|
| Jello modules | `src/wobblertc/` | the `Peer` prototype, enums, events, metrics and helpers |
| The native library | `jdll/wobblertc.c` | a handle for each peer, its event queue, and checks on its input |
| libpeer | `jdll/third_party/libpeer/`, patched by `jdll/patches/` | ICE, DTLS-SRTP, SCTP, RTP, SDP, WHIP and MQTT |
| libpeer's libraries | `jdll/third_party/libpeer/third_party/` | Mbed TLS, libsrtp, usrsctp, cJSON, coreHTTP, llhttp and coreMQTT |

`Peer.jello` binds each native export once, with a typed `let`, on a private abstract `Handle` type, so a program never sees the handle or the natives. The modules turn what the natives return, packed bytes and numbers, into records and enums: `poll` decodes event records into `Event`s, `metrics` reads ten I64s into a `Metrics`, and `state` maps libpeer's numbers to `State`.

## Events and threads

libpeer does its work inside its own calls: `peer_connection_loop` reads the socket, runs ICE and DTLS, and calls the peer's callbacks for state changes, media and messages. usrsctp, which carries the data channel, runs threads of its own and calls the data channel callbacks from them. No callback may touch the VM, so each one only copies what it is given onto the peer's queue, under the peer's lock.

`poll` is a blocking export: it runs on Jello's blocking pool, so the scheduler carries on with other processes meanwhile. It runs libpeer's loop, and the MQTT loop if the peer signals over MQTT, until the queue holds events or the timeout has passed, sleeping 2 milliseconds between idle turns. It then takes events off the queue, oldest first, up to 4 MiB a poll, and packs them for Jello: for each, three little-endian I32s, the kind, a number and a length, then that many bytes.

A queue holds at most 4096 events. When it is full the oldest is dropped, and the next poll starts with an `Event.Dropped` that counts them. The other calls that can wait on the network are blocking exports too: `createOffer`, `createAnswer`, `setRemoteDescription`, `addIceCandidate`, `signal`, `stopSignalling` and `close`.

## Handles, closing and collection

A peer is a native handle whose payload holds the libpeer `PeerConnection`, the queue, its lock and the metrics. `close` destroys the connection, disconnecting any signalling, and marks the handle closed: a closed handle traps with `TYPE_MISMATCH` when used, and closing again does nothing. A peer that is not closed is destroyed by its finalizer when the garbage collector finds it unreachable; a finalizer must not wait on the network, so it forgets MQTT signalling rather than leaving the broker politely.

The handle class has no `retain`: libpeer's objects must stay on one thread at a time, so a peer cannot be sent to another process, and trying traps with `UNSENDABLE`.

libpeer is initialised once a process, on the first peer, and never torn down.

## What wobblertc patches in libpeer

wobblertc builds libpeer at a pinned commit, with five fixes of its own. They are patches in `jdll/patches/`, applied in order; each starts with what it changes and why. Like libpeer, they are under the MIT licence.

| Patch | What was wrong | Now |
|-------|----------------|-----|
| `0001-check-parser-input.patch` | The parsers trusted their input: description lines were copied into 256-byte buffers, candidate fields read with unbounded `sscanf` into fixed arrays and past the end of their line, the 11th remote candidate written past an array of 10, signalling hosts, paths and tokens copied into fixed fields, and a WHIP answer or MQTT message read as if it ended in a zero byte | Lines are read where they lie, however long; every field is checked against what it goes into and refused, not cut short; 32 remote candidates are kept, and one more is an error; a description that is refused changes nothing |
| `0002-reset-signalling.patch` | The signalling session was a global that was written over, never cleared, and its failures went unreported | Each `signal` starts a session afresh; stopping one forgets its URL and token; failures come back as codes |
| `0003-handshake-errors.patch` | The TLS handshake of signalling retried every error for ever | Only a handshake that wants to read or write again is retried, within a time limit; any other error, or a server that says nothing, ends it |
| `0004-verify-certificates.patch` | Signalling never verified the server's certificate | The certificate must be issued for the URL's host by an authority the system trusts, and with none to trust nothing is connected to |
| `0005-quiet-stdout.patch` | libpeer printed to standard output when a data channel opened | Its output goes through its logging, to standard error, and wobblertc builds it with logging off |

The native library turns libpeer's codes into messages, which the Jello side throws as `Peer.method: ...`. It still checks what libpeer cannot see, a zero byte that would end a C string early, and turns `"\n"` line endings into `"\r\n"`, which libpeer needs.

## Signalling

libpeer keeps one signalling session a process. wobblertc lets one peer at a time hold an MQTT session, and stopping it, or closing the peer, ends it, so that any peer can then signal again, at any URL. WHIP posts the offer once, inside `signal`, and the session ends when `signal` returns; MQTT stays connected, and `poll` on the signalling peer runs it. A peer left to the collector closes its MQTT connection without telling the broker, since a finalizer must not wait on the network.

Signalling always uses TLS. The server's certificate is checked against the certificates in the file `SSL_CERT_FILE` and the directories in `SSL_CERT_DIR`, separated by colons, as OpenSSL finds them, or when neither is set against the first of the usual bundles that holds any: `/etc/ssl/cert.pem` (macOS, Alpine and the BSDs), `/etc/ssl/certs/ca-certificates.crt` (Debian and Ubuntu), `/etc/pki/tls/certs/ca-bundle.crt` (Fedora and RHEL) and `/etc/ssl/ca-bundle.pem` (openSUSE). These are the places Jello's own TLS looks. With no certificates to check against, `signal` throws rather than connect.

## Building

`build.sh` compiles everything with the C compiler alone: Mbed TLS (with `MBEDTLS_SSL_DTLS_SRTP` defined on the command line), libsrtp, usrsctp, cJSON, coreHTTP, llhttp, coreMQTT and libpeer, into `build/obj`, then links them with `jdll/wobblertc.c` into `jdll/wobblertc.jdll`. Vendored code is built with hidden symbols and without warnings, and objects are rebuilt only when their source, the flags or the patches change. libpeer's logging is turned off.

libpeer itself is never changed. Each build copies its sources into `build/libpeer-stage`, applies the patches there with `patch`, and copies only the files that come out different into `build/libpeer/src`, which is what compiles; so a build repeats the last one exactly, and a change to a patch recompiles only what it touches.

In a git checkout, libpeer comes from the `jdll/third_party/libpeer` submodule. In a package fetched as an archive, which has no submodules, `build.sh` clones libpeer at the pinned commit.

libpeer runs on macOS and Linux; `build.sh` refuses Windows.

## Limits

- No audio or video codec is linked: wobblertc carries encoded packets, and a program encodes and decodes them.
- Audio is timestamped for 20-millisecond packets, and video for 30 frames a second.
- At most 5 ICE servers, and 32 remote candidates since the last offer.
- A description holds all the local candidates; libpeer does not trickle them.
- libpeer reports neither packet loss nor round-trip times, so `Metrics` counts only what was sent and received.
- Signalling always uses TLS. Mbed TLS matches a server's certificate to the URL's host by name, not by IP address, so name the server as its certificate does.
- One MQTT signalling session a process at a time.
