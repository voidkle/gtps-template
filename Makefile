# gtps: needs g++ (C++20), libssl-dev and zlib1g-dev; ENet is vendored in third_party/enet
CXX := g++
# ENet (the Growtopia patches) is vendored in third_party/enet
CXXFLAGS := -std=c++20 -O2 -g -Wall -Wno-sign-compare -Ithird_party -Isrc
LIBS := -Lthird_party/enet/lib -lenet -lssl -lcrypto -lz -lpthread

gtps.out: src/main.cpp $(wildcard src/*.hpp)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LIBS)

clean:
	rm -f gtps.out

test/e2e.out: test/e2e.cpp src/proto.hpp src/items.hpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LIBS)

test/extras_test.out: test/extras_test.cpp src/extras.hpp src/proto.hpp src/items.hpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LIBS)

test/bot.out: test/bot.cpp src/proto.hpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LIBS)

tools/gtcap.out: tools/gtcap.cpp src/proto.hpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LIBS)

# gtbot: the Lua-scripted bot client (Lua 5.4 is vendored in lua/)
lua/liblua.a: $(wildcard lua/*.c) $(wildcard lua/*.h)
	cd lua && for f in *.c; do gcc -O2 -DLUA_USE_POSIX -c $$f -o $${f%.c}.o; done && ar rcs liblua.a *.o && rm -f *.o

tools/gtbot.out: tools/gtbot.cpp src/proto.hpp src/items.hpp lua/liblua.a
	$(CXX) $(CXXFLAGS) -Ilua $< -o $@ lua/liblua.a $(LIBS) -ldl
