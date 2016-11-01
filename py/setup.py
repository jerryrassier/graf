from distutils.core import setup, Extension

setup(name="graf",
      version="0.1",
      ext_modules=[Extension("graf",
                             ["grafmodule.c"],
                             extra_link_args=['-lz'])])
