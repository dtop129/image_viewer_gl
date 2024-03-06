CFLAGS = -O3 -Iinclude -Wall $(shell pkg-config --cflags glfw3)
CPPFLAGS = -O3 -Iinclude -Wall -std=c++20
LIBS=$(shell pkg-config --libs glfw3)

.DEFAULT_GOAL := viewer

OBJS=main.o gl3w.o

gl3w.o: gl3w.c makefile
	clang $(CFLAGS) -c $< -o $@

main.o: main.cpp app.hpp shader.hpp loader_thread.hpp makefile
	clang++ $(CPPFLAGS) -c $< -o $@

viewer: $(OBJS)
	clang++ $(OBJS) $(LIBS) -o $@
