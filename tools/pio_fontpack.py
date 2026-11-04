"""PlatformIO pre-build step: (re)builds build/fontpack.bin, which the firmware embeds (board_build.embed_files)."""
import os
import subprocess
import sys

Import("env")  # noqa: F821 (provided by PlatformIO)
root = env.subst("$PROJECT_DIR")  # noqa: F821
subprocess.check_call([sys.executable, os.path.join(root, "tools", "make_fontpack.py")])
