CPPFLAGS = -O3 -Iinclude -Wall -std=c++20 $(shell pkg-config --cflags glfw3)
LIBS=$(shell pkg-config --libs glfw3)

.DEFAULT_GOAL := viewer

OBJS=main.o gl3w.o

gl3w.o: gl3w.c makefile
	clang++ $(CPPFLAGS) -c $< -o $@

main.o: main.cpp app.h shader.h loader_thread.h makefile
	clang++ $(CPPFLAGS) -c $< -o $@

viewer: $(OBJS)
	clang++ $(OBJS) $(LIBS) -o $@
