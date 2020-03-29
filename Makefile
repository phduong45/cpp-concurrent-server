CXX := clang++
CXXFLAGS := -std=c++20 -Wall -Wextra -Wpedantic
TARGET := server

.PHONY: all run clean

all: $(TARGET)

$(TARGET): main.cpp
	$(CXX) $(CXXFLAGS) main.cpp -o $(TARGET)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
