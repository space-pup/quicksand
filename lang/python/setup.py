# setup.py
# ----------
# Build script for the quicksand Python package.
# Only works on linux for now.

import os
from setuptools import setup, Extension, find_packages

# Repo root
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

# -----------------------------------------------------------------
# The compiled extension – note that we *do not* compile the library
# itself, we only link against the pre‑built .so.
# -----------------------------------------------------------------
_quicksand_ext = Extension(
    name="quicksand._quicksand",
    sources=["quicksand/_quicksand.c"],            # our thin wrapper
    include_dirs=[os.path.join(ROOT, "quicksand", "include")],
    library_dirs=[os.path.join(ROOT, "build")],
    libraries=["quicksand"],                       # -> libquicksand.so
    extra_link_args=["-ldl"],  # dl tools
    # "-Wl,-rpath,$ORIGIN"],  # relative linking
    extra_compile_args=["-O2", "-flto", "-march=native", "-mtune=native"],
)

# -----------------------------------------------------------------
# The package metadata
# -----------------------------------------------------------------
setup(
    name="quicksand",
    version="0.2.0",
    description="Python bindings for the quicksand message passing library",
    author="Alec Graves",
    packages=find_packages(),
    ext_modules=[_quicksand_ext],
    # package_data={
    #     "quicksand": ["libquicksand.so"]
    # },
    # include_package_data=True,
    zip_safe=True,
    python_requires=">=3.9",
)
