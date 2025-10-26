# setup.py
# ----------
# Build script for the quicksand Python package.

import pathlib
from setuptools import setup, Extension, find_packages

# Where the pre‑built shared library lives (relative to the root of the repo)
LIB_DIR = pathlib.Path(__file__).parent / ".." / "build"

# -----------------------------------------------------------------
# The compiled extension – note that we *do not* compile the library
# itself, we only link against the pre‑built .so.
# -----------------------------------------------------------------
_quicksand_ext = Extension(
    name="quicksand._quicksand",
    sources=["quicksand/_quicksand.c"],      # our thin wrapper
    include_dirs=["../quicksand/include"],              # for quicksand.h
    library_dirs=[str(LIB_DIR)],
    libraries=["quicksand"],                 # -> libquicksand.so
    extra_link_args=["-ldl"],                # dl tools
    extra_compile_args=["-O3", "-march=native", "-mtune=native"], # optimise the wrapper itself
)

# -----------------------------------------------------------------
# The package metadata
# -----------------------------------------------------------------
setup(
    name="quicksand",
    version="0.1.0",
    description="Python bindings for the quicksand shared‑memory ring buffer",
    author="Alec Graves",
    packages=find_packages(),
    ext_modules=[_quicksand_ext],
    package_data={"quicksand": ["../build/libquicksand.so"]},
    zip_safe=False,
    python_requires=">=3.9",
)
