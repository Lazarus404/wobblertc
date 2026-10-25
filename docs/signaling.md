# Signalling

```jello
import { Protocol, protocol } from wobblertc.Signaling;
```

Two peers must swap their descriptions before they can connect, through something they can both reach: a web server, a message broker, a chat, or a person copying text. That is *signalling*, and WebRTC leaves it to the program. A program can send the descriptions itself, any way it likes, with `createOffer`, `createAnswer` and `setRemoteDescription`. Or a peer can use one of the two protocols libpeer speaks, with `signal`:

| Protocol | URL | What happens |
|----------|-----|--------------|
| WHIP | `https://` or `http://` | `signal` posts the peer's offer to the endpoint and takes the answer it sends back |
| MQTT | `mqtts://` or `mqtt://` | `signal` connects to the broker and waits on a topic for offers, which `poll` answers |

`protocol(url)` says which protocol a URL names, and is null for any other URL.

## WHIP

WHIP, the WebRTC-HTTP ingestion protocol, is how many media servers take a stream: the peer posts an offer, and the server answers.

```jello
import { Peer, defaults } from wobblertc.Peer;
import { AudioCodec } from wobblertc.Codecs;

let stream = fn(endpoint: Bytes, token: Bytes): Peer {
  let peer = new Peer({ ...defaults(), audio: AudioCodec.Opus });
  peer.signal(endpoint, token);     // throws when the endpoint gives no answer
  peer
};
```

The token, if not empty, goes as a bearer token. Once `signal` returns, the peer has its answer and is checking candidates; poll it, and send media once it is `Completed`.

## MQTT

With MQTT, the peer waits on a broker for a remote peer to call it. The URL names the broker and a topic, its path: `mqtts://broker.example.org:8883/devices/kitchen`. The peer subscribes to the topic plus `/invoke`, and answers on the topic plus `/result`, in JSON-RPC: an `offer` call makes the peer offer, an `answer` call gives it the remote answer, `state` asks for its state and `close` closes it. The last part of the path is the peer's MQTT client identifier. The token, if not empty, is the Base64 of `username:password`.

```jello
import { Peer, DataChannel, defaults } from wobblertc.Peer;

let device = new Peer({ ...defaults(), channel: DataChannel.Text });
device.signal("mqtts://broker.example.org/devices/kitchen");
// Poll the device in a loop: each poll also runs the MQTT session.
device.poll(50);
device.stopSignalling();
device.close();
```

`stopSignalling()` leaves the broker; `close()` does too. A peer left to the garbage collector cannot tell the broker it has gone, so close a signalling peer when you are done with it.

## Sessions, TLS and failures

libpeer keeps one signalling session for the whole process, so only one peer at a time can be signalling over MQTT; another peer's `signal` throws until it stops. Each `signal` starts afresh: after `stopSignalling`, or a `signal` that failed, a peer can signal again, at any URL and with any token. The host of a URL is at most 63 bytes, its path at most 127, an MQTT client identifier, the last part of the path, at most 31, and the token at most 255.

Signalling always uses TLS, so an `http://` or `mqtt://` URL must still lead to a TLS server, on port 80 or 1883 unless the URL says otherwise. The server's certificate must be issued for the URL's host by an authority this machine trusts: those of the file `SSL_CERT_FILE` and the directories in `SSL_CERT_DIR`, or else the system's bundle, such as `/etc/ssl/cert.pem` on macOS (see [How it works](architecture.md#signalling)). The host is matched by name, so name the server as its certificate does, not by its IP address. A server whose certificate is not trusted makes `signal` throw, and so does finding no trusted certificates at all.

The answer of a WHIP endpoint, and an answer given over MQTT, are checked as `setRemoteDescription` checks a description; an MQTT answer that is refused gets a JSON-RPC `Invalid params` error back.

`signal` throws, saying why, when:

- the URL or token is malformed, or longer than libpeer keeps;
- the server cannot be reached;
- the TLS handshake fails, or the server says nothing for some seconds;
- the server's certificate is not trusted;
- the MQTT broker refuses the session, or the WHIP endpoint does not answer the offer with `201 Created`, or its answer cannot be read.
