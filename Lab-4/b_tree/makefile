CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2
TARGET   := btree
SOURCES  := btree.cpp

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SOURCES)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET) *.exe

.PHONY: all run clean
