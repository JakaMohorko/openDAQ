"""Compile checks/test_c_header.c against the generated header with MSVC and run it.

    OPENDAQ_BIN=<build>/bin python checks/test_c_header.py

Warnings are errors, so a parameter type the header gets wrong fails the
build; the run then calls the libraries through the generated vtables.
"""

import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.dirname(HERE)
VCVARS = os.environ.get("VCVARS", r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat")


def main():
    out = tempfile.mkdtemp(prefix="opendaq_c_header_")
    inc = os.path.join(TOOL, "output", "bindings", "c")
    src = os.path.join(HERE, "test_c_header.c")
    exe = os.path.join(out, "test_c_header.exe")
    bat = os.path.join(out, "build.bat")
    with open(bat, "w") as f:
        f.write(f'@call "{VCVARS}" >nul\r\n')
        f.write(f'cl /nologo /std:c11 /W4 /WX /wd4996 /I "{inc}" "{src}" /Fo"{out}\\\\" /Fe"{exe}"\r\n')
    r = subprocess.run(["cmd", "/c", bat], capture_output=True, text=True)
    print(r.stdout[-3000:], r.stderr[-2000:])
    if r.returncode:
        print("build failed")
        return 1
    r = subprocess.run([exe], capture_output=True, text=True, env=dict(os.environ))
    print(r.stdout)
    return r.returncode


if __name__ == "__main__":
    sys.exit(main())
