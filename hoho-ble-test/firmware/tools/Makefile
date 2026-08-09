# 호스트 검증 하네스.  실행:  make -C tools  (firmware/ 에서)
CXX      ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2 -I../include
BIN      := ./sim_test

.PHONY: all run clean
all: run

$(BIN): sim_test.cpp ../include/protocol.h ../include/simulator.h
	$(CXX) $(CXXFLAGS) -o $(BIN) sim_test.cpp

run: $(BIN)
	$(BIN)

clean:
	rm -f $(BIN)
