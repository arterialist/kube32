# PlatformIO pre-build hook: regenerate the embedded blob tables from images/.
Import("env")
import subprocess, sys, os
root = env["PROJECT_DIR"]
subprocess.check_call([sys.executable, os.path.join(root, "tools", "embed_assets.py"), "--esp"])
