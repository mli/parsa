CC = g++

PS_PATH = ..
PS_LIB = $(PS_PATH)/build/libps.a
PS_THIRD_PATH = $(PS_PATH)/third_party
PS_THIRD_LIB = $(addprefix $(PS_THIRD_PATH)/lib/, \
	libgflags.a libzmq.a libprotobuf.a libglog.a libz.a libsnappy.a)

CFLAGS=-std=c++11 -ggdb -O3 -I./src -I$(PS_PATH)/src -I$(PS_THIRD_PATH)/include -Wall -Wno-unused-function -finline-functions -Wno-sign-compare

all: parsa

parsa: parsa.pb.o main.o parsa.o double_linked_array.o $(PS_LIB) $(PS_THIRD_LIB)
	$(CC) $(CFLAGS)  -o $@ $^ -pthread

%.pb.cc %.pb.h : %.proto
	${PS_THIRD_PATH}/bin/protoc --cpp_out=./src --proto_path=./src --proto_path=$(PS_PATH)/src $<

%.o: src/%.cc src/*.h src/parsa.proto $(PS_LIB)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf parsa *.o
