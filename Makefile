CXX := clang++
CXXFLAGS := -std=c++20 -Wall -Wextra -Wpedantic
TARGETS := server client

.PHONY: all run clean

all: $(TARGETS)

SERVER_SRCS := main.cpp connection.cpp deadline.cpp http.cpp metrics.cpp net_utils.cpp worker.cpp
SERVER_DEPS := blocking_queue.h connection.h deadline.h http.h metrics.h net_utils.h worker.h

server: $(SERVER_SRCS) $(SERVER_DEPS)
	$(CXX) $(CXXFLAGS) $(SERVER_SRCS) -o server

client: client.cpp net_utils.cpp net_utils.h
	$(CXX) $(CXXFLAGS) client.cpp net_utils.cpp -o client

run: server
	./server

clean:
	rm -f $(TARGETS)
