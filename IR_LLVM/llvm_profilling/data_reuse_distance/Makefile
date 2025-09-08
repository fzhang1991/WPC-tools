PROJECT=data_reuse_dist
UNAME_S := $(shell uname -s)
COMMON_FLAGS=-g -O3 -std=c++1y -I.. -Wall -Wextra -Wno-unused-parameter \
	`llvm-config --cxxflags --ldflags --system-libs --libs`

SRC=DataReuseDist.cpp

.PHONY:
	clean $(PROJECT)

all: $(PROJECT)

$(PROJECT):
	@echo Compiling $(SRC)
	clang++ $(SRC) $(COMMON_FLAGS) -o $(PROJECT)

clean:
	rm -rf $(PROJECT) *.out *.o *.ll *.bc
