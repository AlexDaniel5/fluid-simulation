SRC = main.cpp fluid.cpp sand.cpp wind.cpp lava.cpp ice.cpp renderer.cpp input.cpp ui.cpp sweep.cpp
TARGET = build/fluid-simulation
# Queried separately so a missing SDL2_ttf fails with a clear error about the
# font header, instead of silently taking the core SDL2 flags down with it.
SDL_FLAGS = $(shell pkg-config --cflags --libs sdl2) $(shell pkg-config --cflags --libs SDL2_ttf)

# The menu font is compiled into the binary rather than loaded from disk, so
# there is no asset to lose. Regenerate after swapping the .ttf in assets/.
FONT_TTF = assets/Silkscreen-Bold.ttf

# Windows cross-build. Uses the SDL2 headers bundled in src/include and links
# straight against the DLLs already sitting in build/, so the produced .exe and
# the DLLs it needs end up side by side and the .exe is double-clickable.
#
# SDL2_ttf.dll is the menu font's only cost to this arrangement: the upstream
# build statically bundles FreeType and HarfBuzz, so it adds one file rather
# than a chain of them, and the font itself is compiled into the .exe.
WIN_CXX = x86_64-w64-mingw32-g++
WIN_TARGET = build/fluid-simulation.exe
WIN_SDL = build/SDL2.dll build/SDL2_ttf.dll
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

font: font_silkscreen.h

font_silkscreen.h: $(FONT_TTF)
	@{ \
	  echo "#pragma once"; \
	  echo "// Generated from $(FONT_TTF) by \`make font\` — do not edit by hand."; \
	  echo "//"; \
	  echo "// Silkscreen (c) 2001 The Silkscreen Project Authors, SIL Open Font License"; \
	  echo "// 1.1 — see assets/OFL.txt. It is a pixel font drawn on an 8px grid, which is"; \
	  echo "// why the UI renders it at a multiple of 8 and turns hinting and kerning off."; \
	  echo "//"; \
	  echo "// Embedded rather than loaded from disk so the binary stays self-contained:"; \
	  echo "// the Windows build is meant to be a double-clickable .exe with its DLLs beside"; \
	  echo "// it and nothing else, and a loose .ttf that can go missing would break that."; \
	  xxd -i -n silkscreen_ttf $(FONT_TTF) | sed \
	      -e 's/^unsigned char silkscreen_ttf\[\]/static const unsigned char SILKSCREEN_TTF[]/' \
	      -e 's/^unsigned int silkscreen_ttf_len = \(.*\);/static const unsigned int SILKSCREEN_TTF_LEN = \1;/'; \
	} > $@

.PHONY: all windows font
