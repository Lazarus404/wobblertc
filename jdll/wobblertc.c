/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file wobblertc.c
 * @brief WebRTC peers for Jello, on libpeer.
 *
 * A peer is a handle whose payload holds a libpeer PeerConnection and a queue of the events its
 * callbacks report. libpeer calls those callbacks from inside its own calls, and usrsctp calls
 * the data channel ones from threads of its own, so a callback never touches the VM: it copies
 * what it is given onto the queue, under the payload's lock, and peer_poll, a blocking export,
 * runs libpeer's loop and hands the queue back to Jello as packed records.
 *
 * An event record is three little-endian I32s, the kind, a number and a length, then that many
 * bytes. Metrics are ten little-endian I64s, in the order of the metrics struct.
 *
 * libpeer's objects stay on one thread at a time, so the peer class has no `retain`: a peer stays
 * in the process that made it.
 *
 * libpeer, as jdll/patches leaves it, checks what it parses and says what it refused; this file
 * turns its codes into messages for Jello, and checks only what libpeer cannot see, such as a zero
 * byte that would end a C string early.
 */

#include <jello/jdll.h>

#include "agent.h"
#include "ice.h"
#include "peer.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/** The most events a peer keeps before dropping the oldest. */
#define MAX_EVENTS 4096
/** The most bytes of events one poll hands back; the rest wait for the next. */
#define MAX_POLL_BYTES (4u << 20)
/** The longest audio packet: one RTP packet within libpeer's MTU. */
#define MAX_AUDIO 1200u
/** The most ICE servers libpeer takes. */
#define MAX_ICE_SERVERS 5

/** The kinds of event record, as the Peer module decodes them. */
enum {
  EV_STATE,
  EV_LOCAL_DESCRIPTION,
  EV_AUDIO,
  EV_VIDEO,
  EV_CHANNEL_OPEN,
  EV_CHANNEL_CLOSE,
  EV_MESSAGE,
  EV_KEYFRAME,
  EV_DROPPED,
};

/** One queued event, with its bytes after it. */
typedef struct event {
  struct event* next;
  int32_t kind;
  int32_t number;
  uint32_t len;
  uint8_t data[];
} event;

/** What a peer has sent and received, in bytes and packets or messages. */
typedef struct metrics {
  int64_t audio_sent, audio_packets_sent, audio_received, audio_packets_received;
  int64_t video_sent, video_frames_sent, video_received, video_frames_received;
  int64_t messages_sent, messages_received;
} metrics;

/** A peer: its connection, NULL once closed, and what its callbacks have queued. */
typedef struct wrtc_peer {
  PeerConnection* pc;
  int audio;
  int video;
  int channel;
  /** Copies of the ICE servers' text, which libpeer keeps pointers to. */
  char* ice[MAX_ICE_SERVERS * 3];
  pthread_mutex_t lock;
  /** Below, guarded by lock. */
  event* head;
  event* tail;
  uint32_t count;
  int32_t dropped;
  int state_seen;
  metrics m;
} wrtc_peer;

/* The one MQTT signalling session libpeer keeps, and the peer it drives. */
static pthread_mutex_t g_signal_lock = PTHREAD_MUTEX_INITIALIZER;
static wrtc_peer* g_signal_owner;

static pthread_once_t g_init_once = PTHREAD_ONCE_INIT;
static int g_init_failed;

/** Starts libsrtp and usrsctp, once a process. */
static void init_libpeer(void) {
  g_init_failed = peer_init() != 0;
}

/**
 * Ends the signalling session of a peer, if it has it: with DISCONNECT, by telling the broker,
 * and otherwise, as a finalizer must, by closing its connection without a word.
 */
static void signal_release(wrtc_peer* p, int disconnect) {
  pthread_mutex_lock(&g_signal_lock);
  if (g_signal_owner == p) {
    if (disconnect) {
      peer_signaling_disconnect();
    } else {
      peer_signaling_abandon();
    }
    g_signal_owner = NULL;
  }
  pthread_mutex_unlock(&g_signal_lock);
}

/** Destroys a peer's connection, and with it its callbacks. */
static void peer_destroy(wrtc_peer* p, int disconnect) {
  if (!p->pc) return;
  signal_release(p, disconnect);
  peer_connection_destroy(p->pc);
  p->pc = NULL;
}

/** Frees a peer's payload, when no handle holds it any more. */
static void peer_finalize(void* payload) {
  wrtc_peer* p = payload;
  peer_destroy(p, 0);
  for (event* e = p->head; e;) {
    event* next = e->next;
    free(e);
    e = next;
  }
  for (int i = 0; i < MAX_ICE_SERVERS * 3; i++) free(p->ice[i]);
  pthread_mutex_destroy(&p->lock);
  free(p);
}

static const jello_jdll_handle_class PEER = {"wobblertc peer", peer_finalize, NULL};

/** The open peer of argument 0, or NULL once the call has failed. */
static wrtc_peer* arg_peer(jdlo_ctx* c) {
  wrtc_peer* p = jdl_arg_handle(c, 0, &PEER);
  if (p && !p->pc) {
    jdl_fail(c, "the peer is closed");
    return NULL;
  }
  return p;
}

/** Queues an event, dropping the oldest when the queue is full; the caller holds the lock. */
static void push_locked(wrtc_peer* p, int32_t kind, int32_t number, const void* data, size_t len) {
  /* A record's length is an I32; nothing libpeer hands over comes near it. */
  event* e = len <= INT32_MAX - 12 ? malloc(sizeof *e + len) : NULL;
  if (!e) {
    p->dropped++;
    return;
  }
  e->next = NULL;
  e->kind = kind;
  e->number = number;
  e->len = (uint32_t)len;
  if (len) memcpy(e->data, data, len);
  if (p->tail) {
    p->tail->next = e;
  } else {
    p->head = e;
  }
  p->tail = e;
  if (++p->count > MAX_EVENTS) {
    event* oldest = p->head;
    p->head = oldest->next;
    free(oldest);
    p->count--;
    p->dropped++;
  }
}

/** Queues an event. */
static void push(wrtc_peer* p, int32_t kind, int32_t number, const void* data, size_t len) {
  pthread_mutex_lock(&p->lock);
  push_locked(p, kind, number, data, len);
  pthread_mutex_unlock(&p->lock);
}

/* libpeer's callbacks, each given the peer as its userdata. */

static void on_state(PeerConnectionState state, void* userdata) {
  wrtc_peer* p = userdata;
  pthread_mutex_lock(&p->lock);
  p->state_seen = 1;
  push_locked(p, EV_STATE, (int32_t)state, NULL, 0);
  pthread_mutex_unlock(&p->lock);
}

static void on_local_description(char* sdp, void* userdata) {
  if (sdp) push(userdata, EV_LOCAL_DESCRIPTION, 0, sdp, strlen(sdp));
}

static void on_audio(uint8_t* data, size_t size, void* userdata) {
  wrtc_peer* p = userdata;
  pthread_mutex_lock(&p->lock);
  p->m.audio_received += (int64_t)size;
  p->m.audio_packets_received++;
  push_locked(p, EV_AUDIO, 0, data, size);
  pthread_mutex_unlock(&p->lock);
}

static void on_video(uint8_t* data, size_t size, void* userdata) {
  wrtc_peer* p = userdata;
  pthread_mutex_lock(&p->lock);
  p->m.video_received += (int64_t)size;
  p->m.video_frames_received++;
  push_locked(p, EV_VIDEO, 0, data, size);
  pthread_mutex_unlock(&p->lock);
}

static void on_keyframe(void* userdata) {
  push(userdata, EV_KEYFRAME, 0, NULL, 0);
}

static void on_message(char* msg, size_t len, void* userdata, uint16_t sid) {
  wrtc_peer* p = userdata;
  pthread_mutex_lock(&p->lock);
  p->m.messages_received++;
  push_locked(p, EV_MESSAGE, (int32_t)sid, msg, len);
  pthread_mutex_unlock(&p->lock);
}

static void on_channel_open(void* userdata) {
  push(userdata, EV_CHANNEL_OPEN, 0, NULL, 0);
}

static void on_channel_close(void* userdata) {
  push(userdata, EV_CHANNEL_CLOSE, 0, NULL, 0);
}

/** A copy of LEN bytes of DATA as a C string; NULL when memory runs out. */
static char* cstring(const uint8_t* data, size_t len) {
  char* s = malloc(len + 1);
  if (s) {
    if (len) memcpy(s, data, len);
    s[len] = 0;
  }
  return s;
}

/** libpeer's codec for an audio codec of Jello's AudioCodec, by its tag. */
static MediaCodec audio_codec(int32_t tag) {
  switch (tag) {
    case 1:
      return CODEC_OPUS;
    case 2:
      return CODEC_PCMA;
    case 3:
      return CODEC_PCMU;
    default:
      return CODEC_NONE;
  }
}

/** libpeer's codec for a video codec of Jello's VideoCodec, by its tag. */
static MediaCodec video_codec(int32_t tag) {
  return tag == 1 ? CODEC_H264 : CODEC_NONE;
}

/** libpeer's data channel type for one of Jello's DataChannel, by its tag. */
static DataChannelType channel_type(int32_t tag) {
  return tag == 1 ? DATA_CHANNEL_STRING : tag == 2 ? DATA_CHANNEL_BINARY : DATA_CHANNEL_NONE;
}

/**
 * Copies the ICE servers of argument 3 into a peer and its configuration: a URL, a username and
 * a credential for each, every one ending in a zero byte. 0 when the call has failed.
 */
static int copy_ice_servers(jdlo_ctx* c, wrtc_peer* p, PeerConfiguration* config) {
  const uint8_t* data = jdl_arg_bytes_data(c, 3);
  uint32_t len = jdl_arg_bytes_len(c, 3);
  int field = 0;
  for (uint32_t at = 0; at < len && field < MAX_ICE_SERVERS * 3;) {
    const uint8_t* end = memchr(data + at, 0, len - at);
    size_t n = end ? (size_t)(end - (data + at)) : len - at;
    if (!(p->ice[field] = cstring(data + at, n))) {
      jdl_fail(c, "out of memory");
      return 0;
    }
    field++;
    at += (uint32_t)n + 1;
  }
  for (int i = 0; i + 2 < field; i += 3) {
    /* libpeer reads past the scheme, so a URL without one would send it past the end. */
    if (strncmp(p->ice[i], "stun:", 5) != 0 && strncmp(p->ice[i], "turn:", 5) != 0) {
      jdl_fail(c, "an ICE server's URL must start with stun: or turn:");
      return 0;
    }
    IceServer* server = &config->ice_servers[i / 3];
    server->urls = p->ice[i];
    server->username = p->ice[i + 1][0] ? p->ice[i + 1] : NULL;
    server->credential = p->ice[i + 2][0] ? p->ice[i + 2] : NULL;
  }
  return 1;
}

/**
 * A new peer: arguments are the tags of its audio codec, video codec and data channel, and its
 * ICE servers.
 */
static void wrtc_peer_new(jdlo_ctx* c) {
  pthread_once(&g_init_once, init_libpeer);
  if (g_init_failed) {
    jdl_fail(c, "libsrtp did not start");
    return;
  }
  wrtc_peer* p = calloc(1, sizeof *p);
  if (!p) {
    jdl_fail(c, "out of memory");
    return;
  }
  pthread_mutex_init(&p->lock, NULL);
  PeerConfiguration config;
  memset(&config, 0, sizeof config);
  config.audio_codec = audio_codec(jdl_arg_i32(c, 0));
  config.video_codec = video_codec(jdl_arg_i32(c, 1));
  config.datachannel = channel_type(jdl_arg_i32(c, 2));
  config.onaudiotrack = on_audio;
  config.onvideotrack = on_video;
  config.on_request_keyframe = on_keyframe;
  config.user_data = p;
  p->audio = config.audio_codec != CODEC_NONE;
  p->video = config.video_codec != CODEC_NONE;
  p->channel = config.datachannel != DATA_CHANNEL_NONE;
  if (!copy_ice_servers(c, p, &config)) {
    peer_finalize(p);
    return;
  }
  if (!(p->pc = peer_connection_create(&config))) {
    peer_finalize(p);
    jdl_fail(c, "out of memory");
    return;
  }
  peer_connection_oniceconnectionstatechange(p->pc, on_state);
  peer_connection_onicecandidate(p->pc, on_local_description);
  if (p->channel) {
    peer_connection_ondatachannel(p->pc, on_message, on_channel_open, on_channel_close);
  }
  /* libpeer's PeerConnection, with its buffers, is some tens of kilobytes. */
  jdl_return_handle(c, &PEER, p, sizeof *p + 64 * 1024);
}

/** Closes a peer now, rather than when its handle is collected; closing twice does nothing. */
static void wrtc_peer_close(jdlo_ctx* c) {
  wrtc_peer* p = jdl_arg_handle(c, 0, &PEER);
  if (p) peer_destroy(p, 1);
}

/** The peer's state, as libpeer numbers it; a peer that has not begun is new, not closed. */
static void wrtc_peer_state(jdlo_ctx* c) {
  wrtc_peer* p = arg_peer(c);
  if (!p) return;
  PeerConnectionState state = peer_connection_get_state(p->pc);
  pthread_mutex_lock(&p->lock);
  if (state == PEER_CONNECTION_CLOSED && !p->state_seen) state = PEER_CONNECTION_NEW;
  pthread_mutex_unlock(&p->lock);
  jdl_return_i32(c, (int32_t)state);
}

/** Returns a C string as Bytes. */
static void return_text(jdlo_ctx* c, const char* text) {
  jdl_return_bytes_copy(c, (const uint8_t*)text, text ? (uint32_t)strlen(text) : 0u);
}

/** An offer: the peer's local description, which gathers its candidates first. */
static void wrtc_peer_offer(jdlo_ctx* c) {
  wrtc_peer* p = arg_peer(c);
  if (p) return_text(c, peer_connection_create_offer(p->pc));
}

/** An answer to the remote offer, which gathers the peer's candidates first. */
static void wrtc_peer_answer(jdlo_ctx* c) {
  wrtc_peer* p = arg_peer(c);
  if (p) return_text(c, peer_connection_create_answer(p->pc));
}

_Static_assert(AGENT_MAX_REMOTE_CANDIDATES == 32, "the message below names libpeer's limit");

/** What is wrong with a description or candidate libpeer refused with an ICE_ERROR_ code. */
static const char* ice_problem(int code) {
  switch (code) {
    case ICE_ERROR_TOO_LONG:
      return "a field is longer than libpeer keeps";
    case ICE_ERROR_TOO_MANY:
      return "the peer already has as many remote candidates as libpeer keeps, 32";
    case ICE_ERROR_UNSUPPORTED:
      return "libpeer cannot use the candidate: only UDP host, srflx and relay ones";
    default:
      return "a candidate or fingerprint is malformed";
  }
}

/**
 * Copies a description into a C string with every line ending in "\r\n", as libpeer parses it.
 * The copy, or NULL with *PROBLEM saying why.
 */
static char* description_text(const uint8_t* data, uint32_t len, const char** problem) {
  if (len == 0) {
    *problem = "the description is empty";
    return NULL;
  }
  if (memchr(data, 0, len)) {
    *problem = "the description holds a zero byte";
    return NULL;
  }
  char* out = malloc((size_t)len * 2 + 3);
  if (!out) {
    *problem = "out of memory";
    return NULL;
  }
  size_t n = 0;
  for (uint32_t at = 0; at < len;) {
    const uint8_t* nl = memchr(data + at, '\n', len - at);
    uint32_t end = nl ? (uint32_t)(nl - data) : len;
    uint32_t line_end = end > at && data[end - 1] == '\r' ? end - 1 : end;
    memcpy(out + n, data + at, line_end - at);
    n += line_end - at;
    out[n++] = '\r';
    out[n++] = '\n';
    at = end + 1;
  }
  out[n] = 0;
  return out;
}

/**
 * Sets the remote description: arguments are the peer, the description and 1 for an answer or 0
 * for an offer. Empty Bytes when it was taken; otherwise what is wrong with it.
 */
static void wrtc_peer_set_remote(jdlo_ctx* c) {
  wrtc_peer* p = arg_peer(c);
  if (!p) return;
  const char* problem = NULL;
  char* sdp = description_text(jdl_arg_bytes_data(c, 1), jdl_arg_bytes_len(c, 1), &problem);
  if (sdp) {
    int ret = peer_connection_set_remote_description(
        p->pc, sdp, jdl_arg_i32(c, 2) ? SDP_TYPE_ANSWER : SDP_TYPE_OFFER);
    if (ret != 0) problem = ice_problem(ret);
  }
  free(sdp);
  return_text(c, problem ? problem : "");
}

/**
 * Adds a remote candidate; an empty one, which ends the candidates, adds nothing. Empty Bytes
 * when it was taken; otherwise what is wrong with it.
 */
static void wrtc_peer_add_candidate(jdlo_ctx* c) {
  wrtc_peer* p = arg_peer(c);
  if (!p) return;
  const uint8_t* data = jdl_arg_bytes_data(c, 1);
  uint32_t len = jdl_arg_bytes_len(c, 1);
  while (len > 0 && (data[len - 1] == '\r' || data[len - 1] == '\n')) len--;
  const char* problem = NULL;
  if (len == 0) {
    return_text(c, "");
    return;
  }
  if (memchr(data, 0, len)) {
    problem = "the candidate holds a zero byte";
  } else {
    char* text = cstring(data, len);
    if (!text) {
      jdl_fail(c, "out of memory");
      return;
    }
    int ret = peer_connection_add_ice_candidate(p->pc, text);
    if (ret != 0) problem = ice_problem(ret);
    free(text);
  }
  return_text(c, problem ? problem : "");
}

/** Sends an encoded audio packet; whether it went, which needs the peer to be connected. */
static void wrtc_peer_send_audio(jdlo_ctx* c) {
  wrtc_peer* p = arg_peer(c);
  if (!p) return;
  uint32_t len = jdl_arg_bytes_len(c, 1);
  if (!p->audio || len > MAX_AUDIO) {
    jdl_fail(c, p->audio ? "the audio packet is too long" : "the peer has no audio codec");
    return;
  }
  int sent = peer_connection_send_audio(p->pc, jdl_arg_bytes_data(c, 1), len) == 0;
  if (sent) {
    pthread_mutex_lock(&p->lock);
    p->m.audio_sent += len;
    p->m.audio_packets_sent++;
    pthread_mutex_unlock(&p->lock);
  }
  jdl_return_bool(c, sent);
}

/** Sends an encoded video frame; whether it went, which needs the peer to be connected. */
static void wrtc_peer_send_video(jdlo_ctx* c) {
  wrtc_peer* p = arg_peer(c);
  if (!p) return;
  if (!p->video) {
    jdl_fail(c, "the peer has no video codec");
    return;
  }
  uint32_t len = jdl_arg_bytes_len(c, 1);
  int sent = len > 0 && peer_connection_send_video(p->pc, jdl_arg_bytes_data(c, 1), len) == 0;
  if (sent) {
    pthread_mutex_lock(&p->lock);
    p->m.video_sent += len;
    p->m.video_frames_sent++;
    pthread_mutex_unlock(&p->lock);
  }
  jdl_return_bool(c, sent);
}

/**
 * Sends a message on a data channel stream: arguments are the peer, the message and the stream.
 * Whether it went, which needs the data channel to be connected.
 */
static void wrtc_peer_send(jdlo_ctx* c) {
  wrtc_peer* p = arg_peer(c);
  if (!p) return;
  if (!p->channel) {
    jdl_fail(c, "the peer has no data channel");
    return;
  }
  uint32_t len = jdl_arg_bytes_len(c, 1);
  int32_t sid = jdl_arg_i32(c, 2);
  char empty = 0;
  char* data = len ? (char*)jdl_arg_bytes_data(c, 1) : &empty;
  int sent = sid >= 0 && sid <= UINT16_MAX &&
             peer_connection_datachannel_send_sid(p->pc, data, len, (uint16_t)sid) >= 0;
  if (sent) {
    pthread_mutex_lock(&p->lock);
    p->m.messages_sent++;
    pthread_mutex_unlock(&p->lock);
  }
  jdl_return_bool(c, sent);
}

/**
 * Asks the remote peer to open a data channel stream: arguments are the peer, the channel's
 * label and the stream. Whether the request went, which needs the data channel to be connected.
 */
static void wrtc_peer_open_channel(jdlo_ctx* c) {
  wrtc_peer* p = arg_peer(c);
  if (!p) return;
  if (!p->channel) {
    jdl_fail(c, "the peer has no data channel");
    return;
  }
  int32_t sid = jdl_arg_i32(c, 2);
  char* label = cstring(jdl_arg_bytes_data(c, 1), jdl_arg_bytes_len(c, 1));
  if (!label) {
    jdl_fail(c, "out of memory");
    return;
  }
  char protocol[] = "";
  int sent = sid >= 0 && sid <= UINT16_MAX &&
             peer_connection_create_datachannel_sid(p->pc, DATA_CHANNEL_RELIABLE, 0, 0, label,
                                                    protocol, (uint16_t)sid) >= 0;
  free(label);
  jdl_return_bool(c, sent);
}

/** Milliseconds on a clock that only goes forward. */
static int64_t now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/** Whether a peer's state leaves libpeer's loop nothing to wait on. */
static int idle_state(PeerConnectionState state) {
  return state != PEER_CONNECTION_CHECKING && state != PEER_CONNECTION_CONNECTED &&
         state != PEER_CONNECTION_COMPLETED;
}

/** Writes V as four little-endian bytes at OUT. */
static void put_i32(uint8_t* out, int32_t v) {
  for (int b = 0; b < 4; b++) out[b] = (uint8_t)((uint32_t)v >> (8 * b));
}

/** Hands back the queued events as records, as many as fit in MAX_POLL_BYTES. */
static void return_events(jdlo_ctx* c, wrtc_peer* p) {
  pthread_mutex_lock(&p->lock);
  size_t size = p->dropped ? 12 : 0;
  event* last = NULL;
  for (event* e = p->head; e && size + 12 + e->len <= MAX_POLL_BYTES; e = e->next) {
    size += 12 + e->len;
    last = e;
  }
  /* An event bigger than the limit still goes, alone. */
  if (!last && p->head) {
    last = p->head;
    size += 12 + last->len;
  }
  event* taken = last ? p->head : NULL;
  if (last) {
    p->head = last->next;
    if (!p->head) p->tail = NULL;
    last->next = NULL;
  }
  int32_t dropped = p->dropped;
  p->dropped = 0;
  for (event* e = taken; e; e = e->next) p->count--;
  pthread_mutex_unlock(&p->lock);

  uint8_t* out = malloc(size ? size : 1);
  if (!out) {
    while (taken) {
      event* next = taken->next;
      free(taken);
      taken = next;
    }
    jdl_fail(c, "out of memory");
    return;
  }
  size_t at = 0;
  if (dropped) {
    put_i32(out, EV_DROPPED);
    put_i32(out + 4, dropped);
    put_i32(out + 8, 0);
    at = 12;
  }
  while (taken) {
    event* next = taken->next;
    put_i32(out + at, taken->kind);
    put_i32(out + at + 4, taken->number);
    put_i32(out + at + 8, (int32_t)taken->len);
    if (taken->len) memcpy(out + at + 12, taken->data, taken->len);
    at += 12 + taken->len;
    free(taken);
    taken = next;
  }
  jdl_return_bytes_copy(c, out, (uint32_t)at);
  free(out);
}

/**
 * Runs libpeer's loop, and its signalling session's if this peer has it, until there are events
 * or the milliseconds of argument 1 have passed; the events, as records.
 */
static void wrtc_peer_poll(jdlo_ctx* c) {
  wrtc_peer* p = arg_peer(c);
  if (!p) return;
  int32_t timeout = jdl_arg_i32(c, 1);
  int64_t deadline = now_ms() + (timeout > 0 ? timeout : 0);
  for (;;) {
    peer_connection_loop(p->pc);
    pthread_mutex_lock(&g_signal_lock);
    if (g_signal_owner == p) peer_signaling_loop();
    pthread_mutex_unlock(&g_signal_lock);
    pthread_mutex_lock(&p->lock);
    int ready = p->head != NULL || p->dropped != 0;
    pthread_mutex_unlock(&p->lock);
    if (ready || now_ms() >= deadline) break;
    if (idle_state(peer_connection_get_state(p->pc))) {
      struct timespec pause = {0, 2 * 1000000L};
      nanosleep(&pause, NULL);
    }
  }
  return_events(c, p);
}

/** What the peer has sent and received, as ten little-endian I64s. */
static void wrtc_peer_metrics(jdlo_ctx* c) {
  wrtc_peer* p = arg_peer(c);
  if (!p) return;
  pthread_mutex_lock(&p->lock);
  metrics m = p->m;
  pthread_mutex_unlock(&p->lock);
  int64_t values[10] = {m.audio_sent,         m.audio_packets_sent, m.audio_received,
                        m.audio_packets_received, m.video_sent,     m.video_frames_sent,
                        m.video_received,     m.video_frames_received, m.messages_sent,
                        m.messages_received};
  uint8_t out[sizeof values];
  for (int i = 0; i < 10; i++) {
    for (int b = 0; b < 8; b++) out[i * 8 + b] = (uint8_t)((uint64_t)values[i] >> (8 * b));
  }
  jdl_return_bytes_copy(c, out, sizeof out);
}

/** What went wrong, for a PEER_SIGNALING_ERROR_ code from signalling over MQTT or WHIP. */
static const char* signal_problem(int code, int mqtt) {
  switch (code) {
    case PEER_SIGNALING_ERROR_URL:
      return "the URL or token is malformed, or longer than libpeer keeps: a host of 63 bytes, a "
             "path of 127 and a token of 255, and an MQTT URL needs a topic";
    case PEER_SIGNALING_ERROR_CONNECT:
      return "cannot connect to the server";
    case PEER_SIGNALING_ERROR_TLS:
      return "the TLS handshake with the server failed";
    case PEER_SIGNALING_ERROR_CERTIFICATE:
      return "the server's certificate is not trusted for its host";
    case PEER_SIGNALING_ERROR_NO_CA:
      return "no trusted certificates to verify the server with; set SSL_CERT_FILE";
    case PEER_SIGNALING_ERROR_ANSWER:
      return "the WHIP endpoint's answer cannot be read";
    default:
      return mqtt ? "the MQTT broker refused the session"
                  : "the WHIP endpoint did not answer the offer";
  }
}

/**
 * Signals for a peer: arguments are the peer, the URL and the token. An MQTT URL starts a session
 * that waits for offers from the broker, which poll answers; a WHIP URL posts an offer at once
 * and takes the answer. Empty Bytes when it worked; otherwise why not.
 */
static void wrtc_peer_signal(jdlo_ctx* c) {
  wrtc_peer* p = arg_peer(c);
  if (!p) return;
  const uint8_t* url = jdl_arg_bytes_data(c, 1);
  uint32_t url_len = jdl_arg_bytes_len(c, 1);
  const uint8_t* token = jdl_arg_bytes_data(c, 2);
  uint32_t token_len = jdl_arg_bytes_len(c, 2);
  if (url_len > 0 && memchr(url, 0, url_len)) {
    return_text(c, "the URL holds a zero byte");
    return;
  }
  if (token_len > 0 && memchr(token, 0, token_len)) {
    return_text(c, "the token holds a zero byte");
    return;
  }
  char* url_text = cstring(url, url_len);
  char* token_text = cstring(token, token_len);
  if (!url_text || !token_text) {
    free(url_text);
    free(token_text);
    jdl_fail(c, "out of memory");
    return;
  }
  int mqtt = url_len > 0 && url[0] == 'm';
  const char* problem = NULL;
  pthread_mutex_lock(&g_signal_lock);
  if (g_signal_owner) {
    problem = g_signal_owner == p ? "the peer is already signalling"
                                  : "another peer holds the signalling session";
  } else {
    int ret = peer_signaling_connect(url_text, token_text[0] ? token_text : NULL, p->pc);
    if (ret != 0) {
      problem = signal_problem(ret, mqtt);
    } else if (mqtt) {
      g_signal_owner = p;
    } else {
      /* A WHIP post is over once it returns, and its session with it. */
      peer_signaling_disconnect();
    }
    /* Signalling took over the local description callback; unless an MQTT session goes on, the
     * peer has it back. */
    if (!g_signal_owner) peer_connection_onicecandidate(p->pc, on_local_description);
  }
  pthread_mutex_unlock(&g_signal_lock);
  free(url_text);
  free(token_text);
  return_text(c, problem ? problem : "");
}

/** Ends the peer's MQTT signalling session, if it has it. */
static void wrtc_peer_unsignal(jdlo_ctx* c) {
  wrtc_peer* p = arg_peer(c);
  if (!p) return;
  signal_release(p, 1);
  /* Signalling took over the local description callback; the peer has it back. */
  peer_connection_onicecandidate(p->pc, on_local_description);
}

static const jello_jdll_export EXPORTS[] = {
    {"peer_new", wrtc_peer_new, JELLO_T_DYNAMIC, 4,
     {JELLO_T_I32, JELLO_T_I32, JELLO_T_I32, JELLO_T_BYTES}, 0},
    {"peer_close", wrtc_peer_close, JELLO_T_VOID, 1, {JELLO_T_DYNAMIC}, JELLO_JDLL_BLOCKING},
    {"peer_state", wrtc_peer_state, JELLO_T_I32, 1, {JELLO_T_DYNAMIC}, 0},
    {"peer_offer", wrtc_peer_offer, JELLO_T_BYTES, 1, {JELLO_T_DYNAMIC}, JELLO_JDLL_BLOCKING},
    {"peer_answer", wrtc_peer_answer, JELLO_T_BYTES, 1, {JELLO_T_DYNAMIC}, JELLO_JDLL_BLOCKING},
    {"peer_set_remote", wrtc_peer_set_remote, JELLO_T_BYTES, 3,
     {JELLO_T_DYNAMIC, JELLO_T_BYTES, JELLO_T_I32}, JELLO_JDLL_BLOCKING},
    {"peer_add_candidate", wrtc_peer_add_candidate, JELLO_T_BYTES, 2,
     {JELLO_T_DYNAMIC, JELLO_T_BYTES}, JELLO_JDLL_BLOCKING},
    {"peer_send_audio", wrtc_peer_send_audio, JELLO_T_BOOL, 2,
     {JELLO_T_DYNAMIC, JELLO_T_BYTES}, 0},
    {"peer_send_video", wrtc_peer_send_video, JELLO_T_BOOL, 2,
     {JELLO_T_DYNAMIC, JELLO_T_BYTES}, 0},
    {"peer_send", wrtc_peer_send, JELLO_T_BOOL, 3,
     {JELLO_T_DYNAMIC, JELLO_T_BYTES, JELLO_T_I32}, 0},
    {"peer_open_channel", wrtc_peer_open_channel, JELLO_T_BOOL, 3,
     {JELLO_T_DYNAMIC, JELLO_T_BYTES, JELLO_T_I32}, 0},
    {"peer_poll", wrtc_peer_poll, JELLO_T_BYTES, 2, {JELLO_T_DYNAMIC, JELLO_T_I32},
     JELLO_JDLL_BLOCKING},
    {"peer_metrics", wrtc_peer_metrics, JELLO_T_BYTES, 1, {JELLO_T_DYNAMIC}, 0},
    {"peer_signal", wrtc_peer_signal, JELLO_T_BYTES, 3,
     {JELLO_T_DYNAMIC, JELLO_T_BYTES, JELLO_T_BYTES}, JELLO_JDLL_BLOCKING},
    {"peer_unsignal", wrtc_peer_unsignal, JELLO_T_VOID, 1, {JELLO_T_DYNAMIC}, JELLO_JDLL_BLOCKING},
};

JELLO_JDLL_LIBRARY(wobblertc, EXPORTS);
