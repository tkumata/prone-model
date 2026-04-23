format:
	clang-format -i $(shell git ls-files '*.c' '*.h' '*.cpp')

lint:
	cppcheck --enable=all --inconclusive --std=c11 --error-exitcode=1 src

tidy:
	run-clang-tidy -p . -quiet

check:
	format lint

build:
	~/.pico-sdk/ninja/v1.12.1/ninja -C build
