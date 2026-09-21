# 🌐 lrn-http-proxy

C로 직접 만든 HTTP/1.x GET 프록시입니다. **스레드 풀(worker 8·대기 queue 32)**로 동시 요청을 제한하고, 재사용이 허용된 작은 응답을 **LRU 메모리 캐시**에 저장해 원본 서버 요청 횟수를 줄입니다.

[Proxy Wiki](https://docs.woonyong.com/wiki/computer-systems-network-topic-e8bae755299d/) · [핵심 구현](webproxy-lab/proxy.c) · [통합 테스트](tests/test_proxy.py)

## 데모 (구동모습)

![lrn-http-proxy 데모: 같은 URL 2회 요청, 1회차 cache MISS 0.308s, 2회차 cache HIT 0.000471s](docs/demo.gif)

같은 URL을 두 번 요청한 실제 실행 화면입니다. 1회차는 프록시 로그가 `cache MISS`, 원본 서버 도달, 응답 0.308s이고 2회차는 `cache HIT`, 응답 0.000471s입니다. 원본 서버는 원격 서버를 흉내 내려고 응답마다 300ms를 지연시키는 로컬 파이썬 서버라서, 이 시간 차이는 원본 지연이 있을 때의 값이며 일반적인 속도 향상 배수가 아닙니다. 재현: `make setup && bash scripts/demo_latency.sh` ([slow_origin.py](scripts/slow_origin.py)).

## 문제와 목표

브라우저와 원본 서버 사이에서 요청을 대신 보내는 프록시는 세 가지가 어렵습니다. 여러 클라이언트를 동시에 처리하되 자원을 무한정 쓰지 않을 것, 느리거나 끊기는 원본에 매달리지 않을 것, 캐시가 **틀린 응답을 재사용하지 않을 것**입니다.

- 목표 1: absolute-form `GET http://host:port/path` 요청을 중계한다.
- 목표 2: worker 수·대기 queue·timeout·캐시 크기에 **명시적인 상한**을 둔다.
- 목표 3: 재사용이 허용된 응답만 캐시하고, 원본 서버가 받은 요청 횟수로 캐시 동작(HIT/MISS/만료/LRU)을 **관찰 가능한 값으로 검증**한다.
- 비목표: CONNECT/TLS, chunked framing, POST, HTTP/2·3, WebSocket, 캐시 재검증.

## 결과

| 항목 | 결과 | 재현 |
|---|---|---|
| 같은 URL 3회 요청 | 프록시 로그 `MISS` → `HIT` → `HIT`, 원본에는 **1회만** 도달 | 아래 로그 |
| 응답 시간(원본 300ms 지연 조건) | MISS 0.308s → HIT 0.0004s | `bash scripts/demo_latency.sh` |
| 만료 | 원본 요청 수 `MISS 1` → `HIT 1` → 만료 후 `EXPIRED 2` | `make demo` |
| 통합 테스트 | 8개 통과 (7.5초) | `make test` |
| 동시성 한도 | worker 8, 대기 queue 32 | 프록시 시작 로그 |
| 캐시 한도 | 전체 1,049,000바이트 · 객체 102,400바이트 · 16개 entry | 프록시 시작 로그 |

같은 URL을 세 번 요청했을 때 프록시 stderr입니다.[^env]

```text
GET proxy workers=8 queue=32 timeout_ms=2000 cache_bytes=1049000 object_bytes=102400
cache MISS http://127.0.0.1:8000/index.txt
cache HIT http://127.0.0.1:8000/index.txt
cache HIT http://127.0.0.1:8000/index.txt
```

[^env]: 측정 환경은 Apple M4 / macOS, 원본은 [slow_origin.py](scripts/slow_origin.py)(응답마다 300ms 지연). 응답 시간은 `curl -w %{time_total}`이며 3회 요청에서 0.3075s / 0.0004s / 0.0003s였습니다. 로컬 loopback 값이므로 실제 네트워크의 속도 향상을 뜻하지 않습니다.

## 실행 방법

macOS/Linux의 C compiler, Make, Python 3가 필요합니다.

```sh
make setup   # proxy·tiny 빌드
make demo    # MISS → HIT → EXPIRED 출력
make test    # 통합 테스트 8개
```

직접 요청하려면 다음 명령을 각각 다른 터미널에서 실행합니다.

```sh
make serve
make -C webproxy-lab run-tiny TINY_PORT=8000
curl --noproxy '' -x http://127.0.0.1:8080 http://127.0.0.1:8000/home.html
```

서버는 Ctrl-C로 종료합니다. demo/test는 자신이 만든 프로세스만 종료합니다. 환경 변수 `PROXY_WORKERS`(1~32, 기본 8), `PROXY_TIMEOUT_MS`(50~60000, 기본 2000)로 조정합니다.

## 설계

![요청 흐름도: 대기 queue → 검증 → LRU 캐시 HIT/MISS → 원본 → 캐시 저장 조건](docs/figure.png)

명시적 proxy 요청 → worker → URL·헤더 검증 → 원본 연결 또는 LRU cache → 응답으로 이어집니다. 핵심 결정은 다음과 같습니다([proxy.c](webproxy-lab/proxy.c)).

**동시성 모델: 고정 스레드 풀 + 유한 queue**

- main 스레드는 `accept`한 소켓을 크기 32의 원형 queue에 넣고, 미리 만들어 둔 worker 8개가 조건 변수로 꺼내 처리합니다. 연결마다 스레드를 만들지 않으므로 동시에 처리하는 요청과 스레드 수가 상한을 넘지 않습니다.
- queue가 가득 차면 새 연결을 **닫아 버립니다**(대기열을 무한히 키우지 않고 과부하를 클라이언트에 알림). loopback에만 바인딩합니다.
- 트레이드오프: worker가 8개뿐이라 느린 원본이 worker를 오래 붙잡으면 처리량이 떨어집니다. 그래서 timeout에 상한을 두었습니다.

**timeout**

- TCP connect에는 전체 **deadline**(기본 2초)을 두고 주소 후보를 순회하며 남은 시간만큼만 기다립니다. read/write에는 `SO_RCVTIMEO/SO_SNDTIMEO` 유휴 timeout을 적용합니다.
- 한계: DNS 조회는 connect deadline 밖이며 전체 다운로드 시간의 상한은 아닙니다.

**LRU 캐시**

- 16개 고정 슬롯 배열에 논리 시계(`stamp`)를 두고, 접근할 때마다 `stamp`를 올립니다. 슬롯이나 바이트 한도(1,049,000)를 넘으면 `stamp`가 가장 작은 entry부터 버립니다. 만료된 entry는 조회할 때 버립니다.
- 객체는 102,400바이트 이하, Content-Length가 명확한 200 응답 중 `public, max-age`로 재사용이 허용된 것만 저장합니다. 인증·쿠키·Range·조건부 요청과 Set-Cookie·Vary 응답은 캐시하지 않으며, 끊긴 본문은 저장하지 않습니다. Age·Date·전송 지연과 보관 시간을 남은 수명에 반영합니다.
- 전역 mutex 하나로 캐시를 보호하고, HIT 응답은 데이터를 복사한 뒤 **락을 풀고** 전송합니다. 트레이드오프: 슬롯이 16개라 선형 탐색이 충분히 싸고 구조가 단순하지만, 항목이 많아지면 해시 테이블과 락 분할이 필요합니다.

**거절 정책**: 지원하지 않는 request framing은 400/501, origin framing 오류는 502로 거절합니다. 캐시 정책은 [RFC 9111](https://www.rfc-editor.org/rfc/rfc9111.html)의 일부만 구현합니다. [Tiny](webproxy-lab/tiny/tiny.c)는 통합 테스트의 정적 원본 서버로 씁니다.

## 검증

[![CI](https://github.com/woonyong-choi/lrn-http-proxy/actions/workflows/ci.yml/badge.svg)](https://github.com/woonyong-choi/lrn-http-proxy/actions/workflows/ci.yml) <!-- push 후 URL이 활성화된다. -->

- `make test`: [TCP 통합 테스트](tests/test_proxy.py) 8개가 별도 원본 서버의 **요청 횟수**로 HIT/MISS·만료·LRU 축출·용량 한도, 인증·쿠키 요청의 캐시 우회, 끊긴 원본의 비캐시, 병렬 응답과 반복 disconnect, timeout과 지원하지 않는 framing, Tiny 정적 파일을 확인합니다.
- CI([ci.yml](.github/workflows/ci.yml)): ubuntu-latest에서 `make setup`·`make test`를 실행합니다.

## 배운 점·한계

- "캐시했다"가 아니라 **원본이 몇 번 요청받았는지**로 검증해야 캐시가 틀린 응답을 재사용하는 경우(쿠키·인증·만료)를 잡을 수 있었습니다.
- 동시성은 스레드를 늘리는 문제가 아니라 상한과 과부하 시 동작(queue가 차면 연결 닫기)을 정하는 문제였습니다.
- 학습용 프로그램입니다. 캐시 재검증(`ETag`)·chunked 응답·HTTPS 터널링이 없고, 이미 응답 일부를 전달한 뒤 원본이 끊기면 연결 종료로 처리합니다.

## 출처

**크래프톤 정글 개인 과제(원본: [woonyong-choi/SW_AI-W08-webproxy_lab](https://github.com/woonyong-choi/SW_AI-W08-webproxy_lab), 비공개)에서 시작했고, 종료 후 개인 저장소에서 계속 수정·학습·확장하고 있다.**

- 원본 기간: 2026-04-17 ~ 2026-04-23 (원본 첫·마지막 커밋일). 이 저장소는 2026-04-21의 `964163c`에서 이어 받았습니다.
- 개인 확장: 2026-09-08 ~ 2026-09-22, `git log --author="woonyong" 964163c..HEAD`. 원본은 CS:APP 과제 골격(Tiny·Echo)이었고, 이후 worker 수·timeout 상한, 캐시 가능 응답과 거절할 메시지 형식의 제한, 통합 테스트·데모, CI를 추가했습니다.
- 이전 Echo·Tiny·프록시 실습 자료는 [정리 전 이력](https://github.com/woonyong-choi/lrn-http-proxy/tree/0580e06a40163a42e70b18d065f47687ed9f53bf)에 남아 있습니다. 과제 제공 코드(CS:APP `csapp.c`)의 저작권 표시는 소스에 유지합니다.
