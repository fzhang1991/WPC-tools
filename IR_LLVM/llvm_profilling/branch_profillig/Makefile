PROJECT=branch_profiling
UNAME_S := $(shell uname -s)
COMMON_FLAGS=-g -O3 -std=c++1y -I.. \
	-I `llvm-config --includedir --cxxflags --ldflags --system-libs --libs`

SRC=BranchProfiling.cpp

.PHONY:
	clean $(PROJECT)

$(PROJECT): $(SRC)
	@echo Compiling $(SRC)
	clang++ $(SRC) $(COMMON_FLAGS) -o $(PROJECT)

clean:
	rm -rf $(PROJECT)
