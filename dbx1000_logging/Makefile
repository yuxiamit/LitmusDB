HOSTNAME = $(shell hostname)
PEPPER_DEPS = $(HOME)/pepper_deps

ifeq ($(HOSTNAME), yx.csail.mit.edu)
CC=g++
else
CC=g++
endif

ifeq ($(shell echo $(HOSTNAME) | head -c 2), ip)
CC=g++ # EC2
endif

CFLAGS=-Wall -g -std=c++0x # -rdynamic

.SUFFIXES: .o .cpp .h

SRC_DIRS = ./ ./benchmarks/ ./concurrency_control/ ./storage/ ./system/ ./verification/ ../pequin/pepper/libv/ ../pequin/pepper/storage/ ../pequin/pepper/common/ ../pequin/pepper/include/
INCLUDE = -I. -I./benchmarks -I./concurrency_control -I./storage -I./system -I./include -I./verification -I/usr/lib/jvm/java-14-openjdk-amd64/include -I/usr/lib/jvm/java-14-openjdk-amd64/include/linux -I/usr/include/python2.7 -I../pequin/pepper/include -I$(PEPPER_DEPS)/include -I../pequin/thirdparty/libsnark -I../pequin/thirdparty/libsnark/depends/libff -I../pequin/thirdparty/libsnark/depends/libfqfft -I../pequin/pepper -I/usr/include

CFLAGS += $(INCLUDE) -lrt -lpthread -msse4.2 -march=native -ffast-math -Werror -D_GNU_SOURCE -I/usr/include/python2.7 -I/usr/include/x86_64-linux-gnu/python2.7 -L$(PEPPER_DEPS)/lib -fno-strict-aliasing -Wdate-time -D_FORTIFY_SOURCE=2 -g -fdebug-prefix-map=/build/python2.7-8kFkPd/python2.7-2.7.17=. -fstack-protector-strong -Wformat -Werror=format-security -Wno-error -g -fwrapv -Wall -DDB_HASH_NUM_BITS=256 -DDB_NUM_ADDRESSES=1024 -DFOLDER_TMP=\"/tmp/yux2/tmp_state\" -DRAM_CELL_NUM_BITS=32 -DWITH_PROCPS=OFF -DNO_PROCPS -DFOLDER_STATE=\"../pequin/pepper/prover_verifier_shared\" -mcx16 -DCILKP -DMCX16 -fopenmp -lgomp -lntl -O0 # -D_FORTIFY_SOURCE=1 -O2 -fsanitize=address -fno-omit-frame-pointer # -O3

# -D_FORTIFY_SOURCE=1 # -fsanitize=address # -std=c++11 -fsanitize=address -fno-omit-frame-pointer

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
RELEASE_NAME := $(shell uname -r | cut -d '-' -f 3)
	ifeq ($(RELEASE_NAME),generic)
        LDFLAGS = -Wall -L. -L./libs -g -ggdb -std=c++0x -pthread -lrt # -ljemalloc
	endif
	ifeq ($(RELEASE_NAME),Microsoft) # if Windows Subsystem
		LDFLAGS = -Wall -L. -L./libs -g -ggdb -std=c++0x -pthread -lrt # -ljemalloc
	endif
endif
ifeq ($(UNAME_S),Darwin)
        LDFLAGS = -Wall -L. -g -ggdb -std=c++0x -pthread  -lSystem.B # -ljemalloc 
endif



LDFLAGS += $(CFLAGS) -L../pequin/thirdparty/libsnark/build/libsnark -L../pequin/thirdparty/libsnark/build/depends/libff/libff -L../pequin/thirdparty/libsnark/build/depends -lsnark -lff -lzm -lgmp -lgmpxx -lpapi -lrt -lleveldb -lkyotocabinet -Wl,-rpath,$(PEPPER_DEPS)/lib/ -L/usr/bin/java -L/usr/lib/jvm/java-14-openjdk-amd64/lib -L/usr/lib/jvm/java-14-openjdk-amd64/lib/server -ljvm -L/usr/lib/python2.7/config-x86_64-linux-gnu -L/usr/lib -lpython2.7 -lpthread -ldl  -lutil -lm  -Xlinker -export-dynamic -Wl,-Bsymbolic-functions -ldl -lgomp -lntl

CPPS = $(foreach dir, $(SRC_DIRS), $(wildcard $(dir)*.cpp)) ../pequin/pepper/libv/computation_p.cpp
OBJS = $(CPPS:.cpp=.o)
DEPS = $(CPPS:.cpp=.d)

all:rundb

rundb : $(OBJS)
	$(CC) $(ARCH) -o $@ $^ $(LDFLAGS)

#We don't need to clean up when we're making these targets
NODEPS:=clean
ifeq (0, $(words $(findstring $(MAKECMDGOALS), $(NODEPS))))
    -include $(OBJS:%.o=%.d)
endif

tests: tests/litmus-gadgets-test.cpp verification/litmus-gadgets.hpp
	$(CC) tests/litmus-gadgets-test.cpp $(CFLAGS) $(LDFLAGS) -o tests/litmus-gadgets-test

%.d: %.cpp
	$(CC) $(ARCH) -MM -MT $*.o -MF $@ $(CFLAGS) $<

%.o: %.cpp %.d
	$(CC) $(ARCH) -c $(CFLAGS) -o $@ $<

.PHONY: clean
clean:
	rm -f rundb $(OBJS) $(DEPS)


