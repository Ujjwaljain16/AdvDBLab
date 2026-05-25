CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2
TARGET   := rbtree
SOURCES  := main.cc red-black-tree.cc

all: $(TARGET)

$(TARGET): $(SOURCES) red-black-tree.h
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SOURCES)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all run clean