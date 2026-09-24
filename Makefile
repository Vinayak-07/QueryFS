# QueryFS — build with `make`, run with `./queryfs`
CXX      = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -Iinclude
SRC      = $(wildcard src/*.cpp)
BIN      = queryfs

$(BIN): $(SRC) $(wildcard include/*.hpp include/*.tpp)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(BIN)

clean:
	rm -f $(BIN)

.PHONY: clean
