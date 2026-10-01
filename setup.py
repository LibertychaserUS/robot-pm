"""Build the Cython deploy probe. The probe logic lives in the .pyx."""

from setuptools import Extension, setup
from Cython.Build import cythonize

setup(
    ext_modules=cythonize(
        [
            Extension(
                "robot_pm.deploy",
                ["src/robot_pm/deploy.pyx"],
            )
        ],
        compiler_directives={"language_level": "3"},
    ),
)
