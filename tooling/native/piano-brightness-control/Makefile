CXX = xcrun clang++
CXXFLAGS = -std=c++20 -O2 -Wall -Wextra -Werror
OUT = ../../../out/piano-brightness-control

.PHONY: all test
all: $(OUT)/piano-brightness-control

$(OUT)/piano-brightness-control: main.cpp Control.h
	mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) main.cpp -framework IOKit -framework CoreFoundation -framework CoreGraphics -o $@

test: all
	$(CXX) $(CXXFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=all tests.cpp -o $(OUT)/controls_host
	$(OUT)/controls_host
