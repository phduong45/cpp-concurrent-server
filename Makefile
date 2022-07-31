CXX := clang++
CXXFLAGS := -std=c++20 -Wall -Wextra -Wpedantic
TARGETS := server client

.PHONY: all run clean

all: $(TARGETS)

server: main.cpp blocking_queue.h http.cpp http.h net_utils.cpp net_utils.h
	$(CXX) $(CXXFLAGS) main.cpp http.cpp net_utils.cpp -o server

client: client.cpp net_utils.cpp net_utils.h
	$(CXX) $(CXXFLAGS) client.cpp net_utils.cpp -o client

run: server
	./server

clean:
	rm -f $(TARGETS)
