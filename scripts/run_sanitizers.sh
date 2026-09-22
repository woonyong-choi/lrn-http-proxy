#!/bin/bash
# 통합 테스트를 sanitizer 빌드의 프록시로 다시 돌린다.
# ASan+UBSan: 메모리 안전성·누수·미정의 동작. TSan: 캐시와 queue 의 데이터 경합.
# 프록시 바이너리만 바꿔 끼우고 테스트 코드는 그대로 쓴다.
set -uo pipefail
cd "$(dirname "$0")/.."
CC=${CC:-cc}
BUILD=$(mktemp -d)
WRAPPER=webproxy-lab/proxy
trap 'rm -rf "$BUILD"; rm -f "$WRAPPER"; make -s setup >/dev/null 2>&1' EXIT

# sanitizer 런타임이 이 툴체인에서 실제로 뜨는지 먼저 본다. 인자 없이 실행하면
# proxy 는 usage 를 찍고 상태 1 로 끝난다. 멈추거나 시그널로 죽으면 코드가
# 아니라 런타임 문제이므로 테스트 실패로 보고하지 않는다(2026-09 현재 Apple
# Silicon + macOS 26 이 그렇다. Linux/gcc 와 CI 에서는 정상 동작한다).
runtime_starts() {
  "$1" >/dev/null 2>&1 &
  local pid=$! waited=0
  while kill -0 "$pid" 2>/dev/null; do
    if [ "$waited" -ge 50 ]; then
      kill -9 "$pid" 2>/dev/null
      wait "$pid" 2>/dev/null
      return 1
    fi
    sleep 0.1
    waited=$((waited + 1))
  done
  wait "$pid"
  [ "$?" -eq 1 ]
}

status=0
for mode in address,undefined thread; do
  echo "=== sanitizer: $mode ==="
  if ! "$CC" -g -O1 -fsanitize="$mode" -fno-omit-frame-pointer \
      -o "$BUILD/proxy.bin" webproxy-lab/proxy.c webproxy-lab/rio.c \
      -lpthread -lm; then
    echo "skip: $CC 가 -fsanitize=$mode 를 빌드하지 못했다"
    continue
  fi
  if ! runtime_starts "$BUILD/proxy.bin"; then
    echo "skip: -fsanitize=$mode 런타임이 이 플랫폼($(uname -sm))에서 기동하지 않는다"
    continue
  fi
  printf '#!/bin/sh\nexec %s/proxy.bin "$@"\n' "$BUILD" > "$WRAPPER"
  chmod +x "$WRAPPER"
  ASAN_OPTIONS=detect_leaks=1 \
  UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
  TSAN_OPTIONS=halt_on_error=1 \
    python3 -m unittest discover -s tests || status=1
done
exit $status
