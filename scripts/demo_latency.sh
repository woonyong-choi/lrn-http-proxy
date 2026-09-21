#!/bin/bash
# 같은 URL을 두 번 요청해 MISS(원본 300ms 지연)와 HIT(캐시)의 응답 시간을 비교한다. 사전 조건: make setup
cd "$(dirname "$0")/.."
echo "# lrn-http-proxy — 같은 URL을 2번 요청: MISS(원본 300ms) vs HIT(캐시)"
sleep 2.2
python3 scripts/slow_origin.py 8000 & O=$!
./webproxy-lab/proxy 8080 & P=$!
sleep 2.2
for i in 1 2; do
  echo "\$ curl -x 127.0.0.1:8080 http://127.0.0.1:8000/index.txt"
  curl -s --noproxy '' -x http://127.0.0.1:8080 -o /dev/null -w "  -> HTTP %{http_code}, 응답 시간 %{time_total}s\n" http://127.0.0.1:8000/index.txt
  sleep 2.4
done
kill $O $P 2>/dev/null; wait 2>/dev/null
sleep 1.5
