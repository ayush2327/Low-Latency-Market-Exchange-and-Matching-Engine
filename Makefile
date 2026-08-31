CXX := clang++
CXXFLAGS := -std=c++20 -Wall -Wextra -pedantic -O2 -Iinclude
BENCH_CXXFLAGS := -std=c++20 -Wall -Wextra -pedantic -O3 -march=native -DNDEBUG -Iinclude
TARGET := match_engine
BENCH_TARGET := match_engine_benchmark

SRCS := src/main.cpp \
        src/network.cpp \
        src/exchange.cpp \
        src/ring_buffer.cpp \
        src/order_book.cpp \
        src/engine.cpp \
        src/shutdown.cpp

OBJS := $(SRCS:.cpp=.o)

BENCH_SRCS := benchmark/benchmark.cpp \
              src/exchange.cpp \
              src/ring_buffer.cpp \
              src/order_book.cpp

.PHONY: all benchmark run-benchmark clean

all: $(TARGET)

benchmark: $(BENCH_TARGET)

run-benchmark: $(BENCH_TARGET)
	./$(BENCH_TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $(OBJS) -o $(TARGET)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BENCH_TARGET): $(BENCH_SRCS)
	$(CXX) $(BENCH_CXXFLAGS) $(BENCH_SRCS) -o $(BENCH_TARGET)

clean:
	rm -f $(OBJS) $(TARGET) $(BENCH_TARGET)
