// Copyright (C) 2003-2011 Anders Logg
//
// This file is part of DOLFINx (https://www.fenicsproject.org)
//
// SPDX-License-Identifier:    LGPL-3.0-or-later

#include "timing.h"
#include "Timer.h"
#include <dolfinx/common/Table.h>
#include <dolfinx/common/TimeLogManager.h>

namespace
{
dolfinx::common::Timer __global_timer;
dolfinx::common::Timer __tic_timer;
} // namespace

using namespace dolfinx;
using namespace dolfinx::common;

//-----------------------------------------------------------------------
Table dolfinx::timings(std::set<TimingType> type)
{
  return TimeLogManager::logger().timings(type);
}
//-----------------------------------------------------------------------------
void dolfinx::list_timings(MPI_Comm comm, std::set<TimingType> type)
{
  TimeLogManager::logger().list_timings(comm, type);
}
//-----------------------------------------------------------------------------
std::tuple<std::size_t, double, double, double>
dolfinx::timing(std::string task)
{
  return TimeLogManager::logger().timing(task);
}
//-----------------------------------------------------------------------------
