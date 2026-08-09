SRC = main.cpp fluid.cpp sand.cpp wind.cpp lava.cpp renderer.cpp input.cpp
TARGET = build/fluid-simulation
SDL_FLAGS = $(shell pkg-config --cflags --libs sdl2)

# Windows cross-build. Uses the SDL2 headers bundled in src/include and links
# straight against the SDL2.dll already sitting in build/, so the produced .exe
# and the DLL it needs end up side by side and the .exe is double-clickable.
WIN_CXX = x86_64-w64-mingw32-g++
WIN_TARGET = build/fluid-simulation.exe
WIN_SDL = build/SDL2.dll
# -DSDL_MAIN_HANDLED keeps our own main() as the entry point (the bundled
# SDL2main is an MSVC lib, which MinGW can't use), -mwindows makes it a GUI
# binary so no console opens behind the window, and -static folds in
# libstdc++/libgcc/winpthread so SDL2.dll is the only runtime dependency.
WIN_FLAGS = -Isrc/include -DSDL_MAIN_HANDLED -mwindows -static

all: $(TARGET)

$(TARGET): $(SRC)
	g++ -O2 $^ -o $@ $(SDL_FLAGS)

windows: $(WIN_TARGET)

$(WIN_TARGET): $(SRC)
	$(WIN_CXX) -O2 $(WIN_FLAGS) $^ -o $@ $(WIN_SDL)

.PHONY: all windows
