# Codecs

```jello
import { AudioCodec, VideoCodec, OpusMode, opusToc, opusFrameMicros } from wobblertc.Codecs;
```

A peer carries encoded media as it is: it puts each packet or frame into RTP, secures it, and sends it, and hands back what it receives the same way. wobblertc does not encode or decode audio or video; a program sends packets an encoder made, and gives what it receives to a decoder.

## Audio

| `AudioCodec` | RTP payload type | What `sendAudio` takes |
|--------------|------------------|------------------------|
| `Opus` | 111, `opus/48000/2` | one Opus packet |
| `Pcma` | 8, `PCMA/8000` | G.711 A-law bytes, one per sample: 160 for 20 milliseconds |
| `Pcmu` | 0, `PCMU/8000` | G.711 u-law bytes, likewise |
| `None` | | the peer has no audio |

libpeer stamps each packet 20 milliseconds after the one before, so send one packet every 20 milliseconds, each holding 20 milliseconds of sound. A packet is at most `maxAudioPacket`, 1200 bytes. Opus suits browsers and anything new; PCMA and PCMU suit telephone systems.

`Audio` events carry the remote peer's packets in the same form.

## Video

| `VideoCodec` | RTP payload type | What `sendVideo` takes |
|--------------|------------------|------------------------|
| `H264` | 96, `H264/90000` | an access unit: NAL units, each after a start code |
| `None` | | the peer has no video |

libpeer stamps frames for 30 a second, and splits large NAL units into fragments. When the remote peer loses video it asks for a key frame, and the peer reports `Event.KeyframeRequest`.

## Opus packets

Every Opus packet starts with a table-of-contents byte (RFC 6716, section 3.1), which says how the packet was coded. `opusToc` reads it:

```jello
import { OpusMode, opusToc, opusFrameMicros } from wobblertc.Codecs;

let toc = opusToc("\xfc\x01\x02");
if (toc == null) { "empty" } else {
  // config 31: CELT, fullband, 20 ms; stereo; one frame
  let celt = toc.mode == OpusMode.Celt;
  `${toc.config} ${celt} ${toc.stereo} ${toc.frameCode} ${opusFrameMicros(toc.config)}`
}
```

| Field | Says |
|-------|------|
| `config` | the configuration, 0 to 31: the mode, the bandwidth and the frame size |
| `mode` | `OpusMode.Silk` (configurations 0 to 11), `Hybrid` (12 to 15) or `Celt` (16 to 31) |
| `stereo` | whether the packet is stereo |
| `frameCode` | 0 for one frame, 1 for two of equal size, 2 for two of different sizes, 3 for a counted number |

`opusToc` is null for an empty packet. `opusFrameMicros(config)` is the length of each frame of a configuration in microseconds, from 2500 to 60000.
