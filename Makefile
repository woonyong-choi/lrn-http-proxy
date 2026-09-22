.PHONY: setup demo test test-sanitize bench serve
setup:
	$(MAKE) -C webproxy-lab build-all

demo: setup
	python3 scripts/demo.py

test: setup
	python3 -m unittest discover -s tests -v

bench: setup
	python3 scripts/bench.py

# ASan/UBSan(메모리 안전성·미정의 동작)과 TSan(데이터 경합) 아래에서 같은
# 통합 테스트를 돌린다. clang/gcc 의 sanitizer 가 필요하다.
test-sanitize: setup
	bash scripts/run_sanitizers.sh

serve: setup
	./webproxy-lab/proxy 8080
