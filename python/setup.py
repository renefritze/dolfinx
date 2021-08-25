try:
    from skbuild import setup
except ImportError:
    print(
        "Please update pip, you need pip 10 or greater,\n"
        " or you need to install the PEP 518 requirements in pyproject.toml yourself",
        file=sys.stderr,
    )
    raise

REQUIREMENTS = [
    "numpy",
    "mpi4py",
    "petsc4py",
    "fenics-ffcx>=0.1.0,<0.2.0",
    "fenics-ufl>=2021.1.0,<2021.2.0"
]


setup(
    name="fenics-dolfinx",
    version="0.1.1.dev0",
    author='FEniCS Project',
    description='DOLFINx Python interface',
    license="MIT",
    packages=["dolfinx",
              "dolfinx.fem",
              "dolfinx.wrappers",
              "dolfinx_utils.test"],
    package_data={'dolfinx.wrappers': ['*.h']},
    install_requires=REQUIREMENTS,
    setup_requires=["pybind11"],
    zip_safe=False)


