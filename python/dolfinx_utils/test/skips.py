# Copyright (C) 2014-2014 Aslak Wigdahl Bergersen
#
# This file is part of DOLFINx (https://www.fenicsproject.org)
#
# SPDX-License-Identifier:    LGPL-3.0-or-later
"""Shared skips for unit tests involving DOLFINx."""

from mpi4py import MPI
import pytest

from dolfinx.common import has_petsc_complex

# Skips with respect to parallel or serial
xfail_in_parallel = pytest.mark.xfail(
    MPI.COMM_WORLD.size > 1,
    reason="This test does not yet work in parallel.")
skip_in_parallel = pytest.mark.skipif(
    MPI.COMM_WORLD.size > 1,
    reason="This test should only be run in serial.")

# Skips with respect to the scalar type
skip_if_complex = pytest.mark.skipif(
    has_petsc_complex, reason="This test does not work in complex mode.")
xfail_if_complex = pytest.mark.xfail(
    has_petsc_complex, reason="This test does not work in complex mode.")
