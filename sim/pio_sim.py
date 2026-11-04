"""PlatformIO pre-build step of the simulator env: SDL2 and libcurl from the host, C++ standard for C++ files only (clang
rejects it for C), the path of the font pack that sim/src/device.cpp embeds and of the sim folder."""
import os
import subprocess

Import("env")  # noqa: F821 (provided by PlatformIO)


def tool(*cmd):
    try:
        return subprocess.check_output(cmd, text=True).split()
    except (OSError, subprocess.CalledProcessError):
        return []


root = env.subst("$PROJECT_DIR")  # noqa: F821
sdl_cflags = tool("sdl2-config", "--cflags")
sdl_libs = tool("sdl2-config", "--libs")
if not sdl_cflags:
    raise SystemExit("SDL2 not found: brew install sdl2 (macOS) or apt install libsdl2-dev (Linux)")
env.Append(  # noqa: F821
    CCFLAGS=sdl_cflags,
    CXXFLAGS=["-std=gnu++2a"],
    LINKFLAGS=sdl_libs + ["-lcurl", "-lpthread"],
    CPPDEFINES=[
        ("SIM_FONTPACK_PATH", env.StringifyMacro(os.path.join(root, "build", "fontpack.bin"))),  # noqa: F821
        ("SIM_DIR", env.StringifyMacro(os.path.join(root, "sim"))),  # noqa: F821 (default SD card and screenshots)
    ],
)
