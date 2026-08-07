SRC = main.cpp fluid.cpp sand.cpp wind.cpp lava.cpp renderer.cpp input.cpp
TARGET = build/fluid-simulation
SDL_FLAGS = $(shell pkg-config --cflags --libs sdl2)

all: $(TARGET)

$(TARGET): $(SRC)
	g++ -O2 $^ -o $@ $(SDL_FLAGS)
