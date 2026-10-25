#!/usr/bin/env bash
# Builds jdll/wobblertc.jdll, the native library, for this machine.
#
# libpeer and the libraries it needs (Mbed TLS, libsrtp, usrsctp, cJSON, coreHTTP and coreMQTT)
# are vendored in jdll/third_party/libpeer, a git submodule, and are compiled here from source:
# nothing outside the package is needed but a C compiler and patch, and git when libpeer is not
# there yet, as in a package fetched as an archive, to fetch it. libpeer's own sources are copied
# into build/libpeer and the patches in jdll/patches applied to the copy, in order; the submodule
# is never changed. The first build takes a minute or so; later ones only recompile what changed.
# libpeer runs on macOS and Linux, not on Windows.
#
# The Jello headers come from $JELLO_ROOT/include, else from beside the `jello` on PATH: the
# include/ of an installed Jello, or vm/src/include in a checkout of Jello's repository. CC
# chooses the C compiler (cc by default), CFLAGS adds flags, and JOBS sets how many files compile
# at once.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
PEER="$ROOT/jdll/third_party/libpeer"
DEPS="$PEER/third_party"
PATCHES="$ROOT/jdll/patches"
PATCHED="$ROOT/build/libpeer"
STAGE="$ROOT/build/libpeer-stage"
OBJ="$ROOT/build/obj"
GEN="$ROOT/build/include"

die() {
  echo "build.sh: $*" >&2
  exit 1
}

include_dir() {
  local jello candidate
  if [[ -n "${JELLO_ROOT:-}" ]]; then
    echo "$JELLO_ROOT/include"
    return
  fi
  jello="$(command -v jello)" || die "jello is not on PATH; set JELLO_ROOT to Jello's root"
  for candidate in "$(dirname "$jello")/../include" "$(dirname "$jello")/../src/include"; do
    if [[ -f "$candidate/jello/jdll.h" ]]; then
      echo "$candidate"
      return
    fi
  done
  die "cannot find jello/jdll.h beside $jello; set JELLO_ROOT to Jello's root"
}

# The libpeer commit the submodule pins, for copies of the package that are not git checkouts.
LIBPEER_URL=https://github.com/sepfy/libpeer.git
LIBPEER_COMMIT=9319aa434cb9e893faed0293ba9d2a21eca59c8b

have_libpeer() {
  [[ -f "$PEER/src/peer.c" && -f "$DEPS/mbedtls/library/ssl_tls.c" ]]
}

# Fetches libpeer and its own submodules when they are missing: through the submodule in a git
# checkout, and otherwise, as in a package fetched as an archive, by cloning the pinned commit.
fetch_libpeer() {
  have_libpeer && return
  command -v git >/dev/null || die "libpeer is missing and git is not on PATH to fetch it"
  echo "fetching libpeer and its submodules"
  if git -C "$ROOT" rev-parse --show-toplevel 2>/dev/null | grep -qx "$ROOT"; then
    git -C "$ROOT" submodule update --init --recursive --depth 1 jdll/third_party/libpeer ||
      die "cannot fetch libpeer; run: git submodule update --init --recursive"
  else
    [[ -d "$PEER/.git" ]] || git clone -q "$LIBPEER_URL" "$PEER" || die "cannot clone $LIBPEER_URL"
    git -C "$PEER" checkout -q "$LIBPEER_COMMIT" &&
      git -C "$PEER" submodule update --init --recursive --depth 1 ||
      die "cannot fetch libpeer's submodules into $PEER"
  fi
  have_libpeer || die "jdll/third_party/libpeer is still missing its sources"
}

# Copies libpeer's sources into build/libpeer and applies jdll/patches to them, in order. The
# patches go onto a fresh copy each time, in build/libpeer-stage, and only the files that come out
# different replace those in build/libpeer, so a build repeats the last one exactly and recompiles
# only what a change to libpeer or the patches touched.
patch_libpeer() {
  local patch_file file
  command -v patch >/dev/null || die "patch is not on PATH; it applies jdll/patches to libpeer"
  mkdir -p "$STAGE/src" "$PATCHED/src"
  cp "$PEER"/src/*.c "$PEER"/src/*.h "$STAGE/src/" || die "cannot copy libpeer's sources"
  for patch_file in "$PATCHES"/*.patch; do
    patch -p1 -s -f -d "$STAGE" -i "$patch_file" >/dev/null ||
      die "cannot apply $(basename "$patch_file") to libpeer"
  done
  for file in "$STAGE"/src/*.c "$STAGE"/src/*.h; do
    cmp -s "$file" "$PATCHED/src/${file##*/}" || cp "$file" "$PATCHED/src/" ||
      die "cannot copy $file"
  done
}

INCLUDE="$(include_dir)"
CC="${CC:-cc}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"
case "$(uname -s)" in
  Darwin)
    SHARED=(-bundle)
    SCTP_DEFS=(-D__APPLE_USE_RFC_2292 -DHAVE_SA_LEN -DHAVE_SIN_LEN -DHAVE_SIN6_LEN
      -DHAVE_SCONN_LEN -DHAVE_NET_ROUTE_H)
    ;;
  Linux)
    SHARED=(-fPIC -shared)
    SCTP_DEFS=(-D_GNU_SOURCE -DHAVE_LINUX_IF_ADDR_H -DHAVE_LINUX_RTNETLINK_H)
    ;;
  MINGW* | MSYS* | CYGWIN*) die "libpeer does not support Windows; build on macOS or Linux" ;;
  *) die "libpeer supports only macOS and Linux, not $(uname -s)" ;;
esac
case "$(uname -m)" in
  x86_64 | amd64 | i?86 | arm64 | aarch64) CPU=CPU_CISC ;;
  *) CPU=CPU_RISC ;;
esac

fetch_libpeer
patch_libpeer
mkdir -p "$OBJ" "$GEN/srtp2"

# libpeer includes libsrtp as an installed library, <srtp2/srtp.h>, configured by a config.h.
cp "$DEPS/libsrtp/include/srtp.h" "$GEN/srtp2/srtp.h"
cat >"$GEN/config.h" <<EOF
#define PACKAGE_VERSION "2.4.2"
#define PACKAGE_STRING "libsrtp2 2.4.2"
#define $CPU 1
#define HAVE_ARPA_INET_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_NETINET_IN_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_SYS_SOCKET_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_UNISTD_H 1
#define HAVE_INET_ATON 1
#define HAVE_SIGACTION 1
#define HAVE_USLEEP 1
#define HAVE_UINT8_T 1
#define HAVE_UINT16_T 1
#define HAVE_UINT32_T 1
#define HAVE_UINT64_T 1
#define HAVE_INT32_T 1
#define SIZEOF_UNSIGNED_LONG 8
#define SIZEOF_UNSIGNED_LONG_LONG 8
#define HAVE_INLINE 1
EOF

HTTP="$DEPS/coreHTTP/source"
MQTT="$DEPS/coreMQTT/source"
SRTP="$DEPS/libsrtp"
SCTP="$DEPS/usrsctp/usrsctplib"

# Each group of vendored sources, with the flags it compiles with. They are compiled as their
# authors wrote them, so their warnings are not ours to fix, and their symbols stay hidden.
VENDORED=(-std=gnu11 -O2 -w -fPIC -fvisibility=hidden)
MBEDTLS_FLAGS=(-I"$DEPS/mbedtls/include" -DMBEDTLS_SSL_DTLS_SRTP)
SRTP_FLAGS=(-DHAVE_CONFIG_H -I"$GEN" -I"$SRTP/include" -I"$SRTP/crypto/include")
SCTP_FLAGS=(-I"$SCTP" -D__Userspace__ -DSCTP_SIMPLE_ALLOCATOR -DSCTP_PROCESS_LEVEL_LOCKS -DINET
  -DINET6 -DHAVE_SYS_QUEUE_H -DHAVE_STDATOMIC_H -DHAVE_NETINET_IP_ICMP_H "${SCTP_DEFS[@]}")
PEER_FLAGS=(-I"$PATCHED/src" -I"$GEN" -I"$SRTP/include" -I"$DEPS/mbedtls/include" -I"$DEPS/cJSON"
  -I"$SCTP" -I"$HTTP/include" -I"$HTTP/interface" -I"$HTTP/dependency/3rdparty/llhttp/include"
  -I"$MQTT/include" -I"$MQTT/interface" -DHTTP_DO_NOT_USE_CUSTOM_CONFIG
  -DMQTT_DO_NOT_USE_CUSTOM_CONFIG -DMBEDTLS_SSL_DTLS_SRTP -DLOG_LEVEL=-1)

# Objects are kept between builds; a change of compiler or flags rebuilds them all, and so does a
# change to the patches, which may change libpeer's headers.
SIGNATURE="$CC ${CFLAGS:-} ${VENDORED[*]} ${MBEDTLS_FLAGS[*]} ${SRTP_FLAGS[*]} ${SCTP_FLAGS[*]}"
SIGNATURE+=" ${PEER_FLAGS[*]} $(cat "$PATCHES"/*.patch | cksum)"
FRESH=0
[[ -f "$OBJ/signature" && "$(cat "$OBJ/signature")" == "$SIGNATURE" ]] || FRESH=1

OBJECTS=()
PIDS=()

# Waits for the compiles under way; fails when any did.
drain() {
  local pid failed=0
  for pid in ${PIDS[@]+"${PIDS[@]}"}; do
    wait "$pid" || failed=1
  done
  PIDS=()
  [[ $failed -eq 0 ]] || die "a vendored source did not compile"
}

# Compiles SOURCE with the flags after it into build/obj under NAME, unless the object is newer.
compile() {
  local name="$1" source="$2"
  shift 2
  local object="$OBJ/$name.o"
  OBJECTS+=("$object")
  [[ $FRESH -eq 0 && "$object" -nt "$source" ]] && return
  # shellcheck disable=SC2086
  "$CC" "${VENDORED[@]}" "$@" ${CFLAGS:-} -c "$source" -o "$object.tmp" &&
    mv "$object.tmp" "$object" &
  PIDS+=("$!")
  [[ ${#PIDS[@]} -lt $JOBS ]] || drain
}

for source in "$DEPS"/mbedtls/library/*.c; do
  compile "mbedtls_$(basename "$source" .c)" "$source" "${MBEDTLS_FLAGS[@]}"
done
for source in srtp/srtp.c crypto/cipher/{cipher,cipher_test_cases,null_cipher,aes,aes_icm}.c \
  crypto/hash/{auth,auth_test_cases,null_auth,hmac,sha1}.c \
  crypto/kernel/{alloc,crypto_kernel,err,key}.c crypto/math/datatypes.c \
  crypto/replay/{rdb,rdbx}.c; do
  compile "srtp_$(basename "$source" .c)" "$SRTP/$source" "${SRTP_FLAGS[@]}"
done
for source in "$SCTP"/netinet/*.c "$SCTP"/netinet6/*.c "$SCTP"/user_*.c; do
  compile "sctp_$(basename "$source" .c)" "$source" "${SCTP_FLAGS[@]}"
done
compile cjson "$DEPS/cJSON/cJSON.c"
for source in "$HTTP/core_http_client.c" "$HTTP"/dependency/3rdparty/llhttp/src/*.c \
  "$MQTT"/core_mqtt*.c "$PATCHED"/src/*.c; do
  compile "peer_$(basename "$source" .c)" "$source" "${PEER_FLAGS[@]}"
done
drain
echo "$SIGNATURE" >"$OBJ/signature"

# shellcheck disable=SC2086
"$CC" -std=c11 -O2 -Wall -Wextra ${CFLAGS:-} "${SHARED[@]}" -I"$INCLUDE" -I"$PATCHED/src" \
  "$ROOT/jdll/wobblertc.c" "${OBJECTS[@]}" -lpthread -lm -o "$ROOT/jdll/wobblertc.jdll"
echo "built jdll/wobblertc.jdll"
