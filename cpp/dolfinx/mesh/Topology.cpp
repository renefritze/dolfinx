// Copyright (C) 2006-2020 Anders Logg and Garth N. Wells
//
// This file is part of DOLFINx (https://www.fenicsproject.org)
//
// SPDX-License-Identifier:    LGPL-3.0-or-later

#include "Topology.h"
#include "permutationcomputation.h"
#include "topologycomputation.h"
#include "utils.h"
#include <algorithm>
#include <dolfinx/common/IndexMap.h>
#include <dolfinx/common/log.h>
#include <dolfinx/common/sort.h>
#include <dolfinx/common/utils.h>
#include <dolfinx/fem/ElementDofLayout.h>
#include <dolfinx/graph/AdjacencyList.h>
#include <dolfinx/graph/partition.h>
#include <dolfinx/mesh/Mesh.h>
#include <numeric>
#include <random>
#include <unordered_map>
#include <xtl/xspan.hpp>

using namespace dolfinx;
using namespace dolfinx::mesh;

namespace
{
//-----------------------------------------------------------------------------

/// Compute list of processes sharing the same index
/// @param unknown_idx List of indices on each process
/// @return a map to sharing processes for each index, with the (random)
/// owner as the first in the list
std::unordered_map<std::int64_t, std::vector<int>>
determine_sharing_ranks(MPI_Comm comm,
                        const std::vector<std::int64_t>& unknown_idx)
{
  const int mpi_size = dolfinx::MPI::size(comm);

  // Create a global address space to use with all_to_all post-office
  // algorithm and find the owner of each index within that space
  std::int64_t global_space = 0;
  std::int64_t max_index = 0;
  if (!unknown_idx.empty())
    max_index = *std::max_element(unknown_idx.begin(), unknown_idx.end());
  MPI_Allreduce(&max_index, &global_space, 1, MPI_INT64_T, MPI_SUM, comm);
  global_space += 1;

  std::vector<std::vector<std::int64_t>> send_indices(mpi_size);
  for (std::int64_t global_i : unknown_idx)
  {
    const int index_owner
        = dolfinx::MPI::index_owner(mpi_size, global_i, global_space);
    send_indices[index_owner].push_back(global_i);
  }

  const graph::AdjacencyList<std::int64_t> recv_indices
      = dolfinx::MPI::all_to_all(
          comm, graph::AdjacencyList<std::int64_t>(send_indices));

  // Get index sharing - ownership will be first entry (randomised later)
  std::unordered_map<std::int64_t, std::vector<int>> index_to_owner;
  for (int p = 0; p < recv_indices.num_nodes(); ++p)
  {
    auto recv_p = recv_indices.links(p);
    for (std::size_t j = 0; j < recv_p.size(); ++j)
      index_to_owner[recv_p[j]].push_back(p);
  }

  std::mt19937 g(0);
  // Randomise ownership
  for (auto& map_entry : index_to_owner)
  {
    std::vector<int>& procs = map_entry.second;
    std::shuffle(procs.begin(), procs.end(), g);
  }

  // Send index ownership data back to all sharing processes
  std::vector<std::vector<int>> send_owner(mpi_size);
  for (int p = 0; p < recv_indices.num_nodes(); ++p)
  {
    auto recv_p = recv_indices.links(p);
    for (std::size_t j = 0; j < recv_p.size(); ++j)
    {
      const auto it = index_to_owner.find(recv_p[j]);
      assert(it != index_to_owner.end());
      const std::vector<int>& sharing_procs = it->second;
      send_owner[p].push_back(sharing_procs.size());
      for (int sp : sharing_procs)
        send_owner[p].push_back(sp);
    }
  }

  // Alltoall is necessary because cells which are shared by vertex are
  // not yet known to this process
  const graph::AdjacencyList<int> recv_owner
      = dolfinx::MPI::all_to_all(comm, graph::AdjacencyList<int>(send_owner));

  // Now fill index_to_owner with locally needed indices
  index_to_owner.clear();
  for (int p = 0; p < mpi_size; ++p)
  {
    const std::vector<std::int64_t>& send_v = send_indices[p];
    auto r_owner = recv_owner.links(p);
    std::size_t c(0), i(0);
    while (c < r_owner.size())
    {
      int count = r_owner[c++];
      for (int j = 0; j < count; ++j)
        index_to_owner[send_v[i]].push_back(r_owner[c++]);
      ++i;
    }
  }

  return index_to_owner;
}
//-----------------------------------------------------------------------------
// Start to create a map from the 64-bit input vertex index to a local
// index. Finds the set of vertices which are in ghost cells, and maps
// them to -1. Vertices which only appear in local cells are mapped to
// -2. They will be receive local indexing later. Vertex indices which
// are both in local cells, and in ghost cells (ownership yet unknown)
// are returned as a vector.
// @param cells Input mesh topology
// @param num_local_cells Number of local (non-ghost) cells
// @return (global_to_local_map, unknown_indices)
std::pair<std::unordered_map<std::int64_t, std::int32_t>,
          std::vector<std::int64_t>>
create_sets(const graph::AdjacencyList<std::int64_t>& cells,
            int num_local_cells)
{
  common::Timer t0("Topology: create sets");

  // Build a set of 'local' cell vertices
  std::vector<std::int64_t> local_vertex_set(
      cells.array().begin(),
      std::next(cells.array().begin(), cells.offsets()[num_local_cells]));
  dolfinx::radix_sort(xtl::span(local_vertex_set));
  local_vertex_set.erase(
      std::unique(local_vertex_set.begin(), local_vertex_set.end()),
      local_vertex_set.end());

  // Build a set of ghost cell vertices
  std::vector<std::int64_t> ghost_vertex_set(
      std::next(cells.array().begin(), cells.offsets()[num_local_cells]),
      cells.array().end());
  dolfinx::radix_sort(xtl::span(ghost_vertex_set));
  ghost_vertex_set.erase(
      std::unique(ghost_vertex_set.begin(), ghost_vertex_set.end()),
      ghost_vertex_set.end());

  // Compute the intersection of local cell vertices and ghost cell
  // vertices

  // Any vertices which are in ghost cells set to -1
  std::unordered_map<std::int64_t, std::int32_t> global_to_local_vertices;
  for (std::int64_t idx : ghost_vertex_set)
    global_to_local_vertices.insert({idx, -1});

  std::vector<std::int64_t> unknown_indices_set;
  for (std::int64_t global_index : local_vertex_set)
  {
    // Check if already in a ghost cell
    if (const auto it = global_to_local_vertices.find(global_index);
        it != global_to_local_vertices.end())
    {
      unknown_indices_set.push_back(global_index);
    }
    else
    {
      // This vertex is not shared: set to -2
      auto [it_ignore, insert]
          = global_to_local_vertices.insert({global_index, -2});
      assert(insert);
    }
  }

  return {std::move(global_to_local_vertices), std::move(unknown_indices_set)};
}
//-----------------------------------------------------------------------------
// Compute a neighborhood comm from the values in
// global_vertex_to_ranks, also returning the map from process number to
// neighbor number Input params as in mesh::create_topology()
// @return (neighbor comm, global rank to neighborhood rank)
std::pair<MPI_Comm, std::map<int, int>>
compute_neighbor_comm(const MPI_Comm& comm, int mpi_rank,
                      const std::unordered_map<std::int64_t, std::vector<int>>&
                          global_vertex_to_ranks)
{
  // Create set of all ranks that share a vertex with this rank. Note
  // this can be 'wider' than the neighbor comm of shared cells.
  std::vector<int> neighbors;
  std::for_each(
      global_vertex_to_ranks.begin(), global_vertex_to_ranks.end(),
      [&neighbors](auto& q)
      { neighbors.insert(neighbors.end(), q.second.begin(), q.second.end()); });
  std::sort(neighbors.begin(), neighbors.end());
  neighbors.erase(std::unique(neighbors.begin(), neighbors.end()),
                  neighbors.end());
  auto it = std::find(neighbors.begin(), neighbors.end(), mpi_rank);
  neighbors.erase(it); // Remove my rank

  // Build map from neighbor global rank to neighbor local rank
  std::map<int, int> rank_global_to_neighborhood;
  for (std::size_t i = 0; i < neighbors.size(); ++i)
    rank_global_to_neighborhood.insert({neighbors[i], i});

  // Create neighborhood communicator
  MPI_Comm neighbor_comm;
  MPI_Dist_graph_create_adjacent(comm, neighbors.size(), neighbors.data(),
                                 MPI_UNWEIGHTED, neighbors.size(),
                                 neighbors.data(), MPI_UNWEIGHTED,
                                 MPI_INFO_NULL, false, &neighbor_comm);

  return {neighbor_comm, std::move(rank_global_to_neighborhood)};
}
//-------------------------------------------------------------------------------
// Send the vertex number for owned vertices to ranks that also share
// them, and return a list of triplets received from other ranks. Each
// triplet consists of {old_global_vertex_index,
// new_global_vertex_index, owning_rank}. The received vertices will be
// "ghost" on this process.
// Input params as in mesh::create_topology()
// @return list of triplets
std::vector<std::int64_t>
send_vertex_numbering(const MPI_Comm& neighbor_comm,
                      const std::map<int, int>& global_to_neighbor_rank,
                      int mpi_rank,
                      const std::unordered_map<std::int64_t, std::vector<int>>&
                          global_vertex_to_ranks,
                      std::int64_t global_offset_v,
                      const std::unordered_map<std::int64_t, std::int32_t>&
                          global_to_local_vertices)
{
  // Pack send data
  std::vector<std::vector<std::int64_t>> send_triplets(
      global_to_neighbor_rank.size());
  for (auto& vertex : global_vertex_to_ranks)
  {
    // Get ranks that share this vertex
    const std::vector<int>& sharing_ranks = vertex.second;
    if (sharing_ranks[0]
        == mpi_rank) // FIXME: this isn't clear. What's the precondition?
    {
      const auto it = global_to_local_vertices.find(vertex.first);
      assert(it != global_to_local_vertices.end());
      assert(it->second != -1);

      // Owned and shared with these processes
      // Note: starting from 1, 0 is self
      for (std::size_t j = 1; j < sharing_ranks.size(); ++j)
      {
        auto ranks = global_to_neighbor_rank.find(sharing_ranks[j]);
        assert(ranks != global_to_neighbor_rank.end());
        const int neigh_rank = ranks->second;
        send_triplets[neigh_rank].push_back(it->first);
        send_triplets[neigh_rank].push_back(it->second + global_offset_v);
        send_triplets[neigh_rank].push_back(mpi_rank);
      }
    }
  }

  return dolfinx::MPI::neighbor_all_to_all(
             neighbor_comm, graph::AdjacencyList<std::int64_t>(send_triplets))
      .array();
}
//---------------------------------------------------------------------
// Send vertex numbering of vertices in ghost cells to neighbours. These
// include vertices that were numbered remotely and received in a
// previous round. This is only needed for meshes with shared cells,
// i.e. ghost_mode=shared_facet. Returns a list of triplets,
// {old_global_vertex_index, new_global_vertex_index, owner}.
// Input params as in mesh::create_topology()
// @return list of triplets
std::vector<std::int64_t> send_ghost_vertex_numbering(
    MPI_Comm neighbor_comm, int mpi_rank,
    const std::map<int, int>& proc_to_neighbors,
    std::shared_ptr<const common::IndexMap> index_map_c,
    const graph::AdjacencyList<std::int64_t>& cells, int nlocal,
    std::int64_t global_offset_v,
    const std::unordered_map<std::int64_t, std::int32_t>&
        global_to_local_vertices,
    const std::vector<std::int64_t>& ghost_vertices,
    const std::vector<int>& ghost_vertex_owners)
{
  // Receive index of ghost vertices that are not on the process
  // boundary from the ghost cell owner. Note: the ghost cell owner
  // might not be the same as the vertex owner.

  std::map<std::int64_t, std::set<std::int32_t>> fwd_shared_vertices;
  const graph::AdjacencyList<std::int32_t>& fwd_shared_cells
      = index_map_c->scatter_fwd_indices();

  std::vector<int> fwd_procs;
  std::tie(fwd_procs, std::ignore) = dolfinx::MPI::neighbors(
      index_map_c->comm(common::IndexMap::Direction::forward));

  const int num_local_cells = index_map_c->size_local();
  for (int p = 0; p < fwd_shared_cells.num_nodes(); ++p)
  {
    for (std::int32_t c : fwd_shared_cells.links(p))
    {
      if (c < num_local_cells)
      {
        // Vertices in local cells that are shared forward
        for (std::int32_t v : cells.links(c))
          fwd_shared_vertices[v].insert(fwd_procs[p]);
      }
    }
  }

  // Precompute sizes and offsets
  std::vector<int> send_sizes(proc_to_neighbors.size()),
      sdispl(proc_to_neighbors.size() + 1);
  for (const auto& q : fwd_shared_vertices)
  {
    for (int p : q.second)
    {
      const auto p_it = proc_to_neighbors.find(p);
      assert(p_it != proc_to_neighbors.end());
      send_sizes[p_it->second] += 3;
    }
  }
  std::partial_sum(send_sizes.begin(), send_sizes.end(), sdispl.begin() + 1);
  std::vector<int> tmp_offsets(sdispl.begin(), sdispl.end());

  // Fill data for neighbor alltoall
  std::vector<std::int64_t> send_triplet_data(sdispl.back());
  for (const auto& q : fwd_shared_vertices)
  {
    const auto it = global_to_local_vertices.find(q.first);
    assert(it != global_to_local_vertices.end());
    assert(it->second != -1);
    const std::int64_t gi = it->second < nlocal
                                ? it->second + global_offset_v
                                : ghost_vertices[it->second - nlocal];
    const int owner_rank = it->second < nlocal
                               ? mpi_rank
                               : ghost_vertex_owners[it->second - nlocal];

    for (int p : q.second)
    {
      const auto p_it = proc_to_neighbors.find(p);
      assert(p_it != proc_to_neighbors.end());
      const int np = p_it->second;
      send_triplet_data[tmp_offsets[np]++] = q.first;
      send_triplet_data[tmp_offsets[np]++] = gi;
      send_triplet_data[tmp_offsets[np]++] = owner_rank;
    }
  }

  return dolfinx::MPI::neighbor_all_to_all(
             neighbor_comm,
             graph::AdjacencyList<std::int64_t>(send_triplet_data, sdispl))
      .array();
}
//---------------------------------------------------------------------------------
graph::AdjacencyList<std::int32_t> convert_cells_to_local_indexing(
    mesh::GhostMode ghost_mode, const graph::AdjacencyList<std::int64_t>& cells,
    std::int32_t num_local_cells,
    const std::unordered_map<std::int64_t, std::int32_t>
        global_to_local_vertices)
{
  std::vector<std::int32_t> local_offsets;
  if (ghost_mode == mesh::GhostMode::none)
  {
    // Discard ghost cells
    local_offsets.assign(
        cells.offsets().begin(),
        std::next(cells.offsets().begin(), num_local_cells + 1));
  }
  else
    local_offsets.assign(cells.offsets().begin(), cells.offsets().end());

  std::vector<std::int32_t> cells_array_local(local_offsets.back());
  std::transform(
      cells.array().begin(), cells.array().begin() + cells_array_local.size(),
      cells_array_local.begin(),
      [&](std::int64_t i) { return global_to_local_vertices.at(i); });

  return graph::AdjacencyList<std::int32_t>(std::move(cells_array_local),
                                            std::move(local_offsets));
}
} // namespace

//-----------------------------------------------------------------------------
std::vector<bool> mesh::compute_boundary_facets(const Topology& topology)
{
  const int tdim = topology.dim();

  auto facets = topology.index_map(tdim - 1);
  if (!facets)
    throw std::runtime_error("Facets have not been computed.");
  std::set<std::int32_t> fwd_shared_facets;
  if (facets->num_ghosts() == 0)
  {
    fwd_shared_facets
        = std::set<std::int32_t>(facets->scatter_fwd_indices().array().begin(),
                                 facets->scatter_fwd_indices().array().end());
  }

  std::shared_ptr<const graph::AdjacencyList<std::int32_t>> fc
      = topology.connectivity(tdim - 1, tdim);
  if (!fc)
    throw std::runtime_error("Facet-cell connectivity missing.");
  std::vector<bool> _boundary_facet(facets->size_local(), false);
  for (std::size_t f = 0; f < _boundary_facet.size(); ++f)
  {
    if (fc->num_links(f) == 1
        and fwd_shared_facets.find(f) == fwd_shared_facets.end())
    {
      _boundary_facet[f] = true;
    }
  }

  return _boundary_facet;
}
//-----------------------------------------------------------------------------
Topology::Topology(MPI_Comm comm, mesh::CellType type)
    : _mpi_comm(comm), _cell_type(type),
      _connectivity(
          mesh::cell_dim(type) + 1,
          std::vector<std::shared_ptr<graph::AdjacencyList<std::int32_t>>>(
              mesh::cell_dim(type) + 1))
{
  // Do nothing
}
//-----------------------------------------------------------------------------
int Topology::dim() const noexcept { return _connectivity.size() - 1; }
//-----------------------------------------------------------------------------
void Topology::set_index_map(int dim,
                             const std::shared_ptr<const common::IndexMap>& map)
{
  assert(dim < (int)_index_map.size());
  _index_map[dim] = map;
}
//-----------------------------------------------------------------------------
std::shared_ptr<const common::IndexMap> Topology::index_map(int dim) const
{
  assert(dim < (int)_index_map.size());
  return _index_map[dim];
}
//-----------------------------------------------------------------------------
std::int32_t Topology::create_entities(int dim)
{
  // TODO: is this check sufficient/correct? Does not catch the cell_entity
  // entity case. Should there also be a check for
  // connectivity(this->dim(), dim) ?
  // Skip if already computed (vertices (dim=0) should always exist)
  if (connectivity(dim, 0))
    return -1;

  // Create local entities
  const auto [cell_entity, entity_vertex, index_map]
      = mesh::compute_entities(_mpi_comm.comm(), *this, dim);

  if (cell_entity)
    set_connectivity(cell_entity, this->dim(), dim);

  // TODO: is this check necessary? Seems redundant after to the "skip check"
  if (entity_vertex)
    set_connectivity(entity_vertex, dim, 0);

  assert(index_map);
  this->set_index_map(dim, index_map);

  return index_map->size_local();
}
//-----------------------------------------------------------------------------
void Topology::create_connectivity(int d0, int d1)
{
  // Make sure entities exist
  create_entities(d0);
  create_entities(d1);

  // Compute connectivity
  const auto [c_d0_d1, c_d1_d0] = mesh::compute_connectivity(*this, d0, d1);

  // NOTE: that to compute the (d0, d1) connections is it sometimes
  // necessary to compute the (d1, d0) connections. We store the (d1,
  // d0) for possible later use, but there is a memory overhead if they
  // are not required. It may be better to not automatically store
  // connectivity that was not requested, but advise in a docstring the
  // most efficient order in which to call this function if several
  // connectivities are needed.

  // TODO: Caching policy/strategy.
  // Concerning the note above: Provide an overload
  // create_connectivity(std::vector<std::pair<int, int>>)?

  // Attach connectivities
  if (c_d0_d1)
    set_connectivity(c_d0_d1, d0, d1);
  if (c_d1_d0)
    set_connectivity(c_d1_d0, d1, d0);
}
//-----------------------------------------------------------------------------
void Topology::create_entity_permutations()
{
  if (!_cell_permutations.empty())
    return;

  const int tdim = this->dim();

  // FIXME: Is this always required? Could it be made cheaper by doing a
  // local version? This call does quite a lot of parallel work
  // Create all mesh entities

  for (int d = 0; d < tdim; ++d)
    create_entities(d);

  auto [facet_permutations, cell_permutations]
      = mesh::compute_entity_permutations(*this);
  _facet_permutations = std::move(facet_permutations);
  _cell_permutations = std::move(cell_permutations);
}
//-----------------------------------------------------------------------------
std::shared_ptr<const graph::AdjacencyList<std::int32_t>>
Topology::connectivity(int d0, int d1) const
{
  assert(d0 < (int)_connectivity.size());
  assert(d1 < (int)_connectivity[d0].size());
  return _connectivity[d0][d1];
}
//-----------------------------------------------------------------------------
void Topology::set_connectivity(
    std::shared_ptr<graph::AdjacencyList<std::int32_t>> c, int d0, int d1)
{
  assert(d0 < (int)_connectivity.size());
  assert(d1 < (int)_connectivity[d0].size());
  _connectivity[d0][d1] = c;
}
//-----------------------------------------------------------------------------
const std::vector<std::uint32_t>& Topology::get_cell_permutation_info() const
{
  if (_cell_permutations.empty())
  {
    throw std::runtime_error(
        "create_entity_permutations must be called before using this data.");
  }
  return _cell_permutations;
}
//-----------------------------------------------------------------------------
const std::vector<std::uint8_t>& Topology::get_facet_permutations() const
{
  if (_facet_permutations.empty())
  {
    throw std::runtime_error(
        "create_entity_permutations must be called before using this data.");
  }
  return _facet_permutations;
}
//-----------------------------------------------------------------------------
mesh::CellType Topology::cell_type() const noexcept { return _cell_type; }
//-----------------------------------------------------------------------------
MPI_Comm Topology::mpi_comm() const { return _mpi_comm.comm(); }
//-----------------------------------------------------------------------------
Topology
mesh::create_topology(MPI_Comm comm,
                      const graph::AdjacencyList<std::int64_t>& cells,
                      const xtl::span<const std::int64_t>& original_cell_index,
                      const xtl::span<const int>& ghost_owners,
                      const CellType& cell_type, mesh::GhostMode ghost_mode)
{
  LOG(INFO) << "Create topology";
  if (cells.num_nodes() > 0
      and cells.num_links(0) != mesh::num_cell_vertices(cell_type))
  {
    throw std::runtime_error(
        "Inconsistent number of cell vertices. Got "
        + std::to_string(cells.num_links(0)) + ", expected "
        + std::to_string(mesh::num_cell_vertices(cell_type)) + ".");
  }

  const int mpi_rank = dolfinx::MPI::rank(comm);

  // Create index map for cells
  const std::int32_t num_local_cells = cells.num_nodes() - ghost_owners.size();
  std::shared_ptr<common::IndexMap> index_map_c;
  if (ghost_mode == mesh::GhostMode::none)
    index_map_c = std::make_shared<common::IndexMap>(comm, num_local_cells);
  else
  {
    // Get global indices of ghost cells
    const std::vector cell_ghost_indices = graph::build::compute_ghost_indices(
        comm, original_cell_index, ghost_owners);
    index_map_c = std::make_shared<common::IndexMap>(
        comm, num_local_cells,
        dolfinx::MPI::compute_graph_edges(
            comm, std::set<int>(ghost_owners.begin(), ghost_owners.end())),
        cell_ghost_indices, ghost_owners);
  }

  // Creating a global-to-local map, labelling local unshared vertices
  // with -2, and other vertices (ghost or unknown owner) with -1.
  // Return list of vertices whose ownership still needs determining.
  auto [global_to_local_vertices, unknown_indices_set]
      = create_sets(cells, num_local_cells);

  // For each vertex whose ownership needs determining, compute list of
  // sharing ranks
  std::unordered_map<std::int64_t, std::vector<int>> global_vertex_to_ranks
      = determine_sharing_ranks(comm, unknown_indices_set);

  // Take ownership of vertices based on global_vertex_to_ranks
  for (std::int64_t global_index : unknown_indices_set)
  {
    const auto it = global_vertex_to_ranks.find(global_index);
    assert(it != global_vertex_to_ranks.end());

    // Vertex is shared and locally owned if first owning rank is my
    // rank
    if (it->second[0] == mpi_rank)
    {
      // Should already be in map
      auto it_gi = global_to_local_vertices.find(global_index);
      assert(it_gi != global_to_local_vertices.end());
      assert(it_gi->second == -1);
      // Mark as locally owned
      it_gi->second = -2;
    }
  }

  // Number all locally owned vertices, iterating cellwise
  std::int32_t v = 0;
  for (std::int32_t c = 0; c < cells.num_nodes(); ++c)
  {
    for (auto vtx : cells.links(c))
    {
      auto it = global_to_local_vertices.find(vtx);
      assert(it != global_to_local_vertices.end());
      if (it->second == -2)
        it->second = v++;
    }
  }

  // Compute the global offset for local vertex indices
  const std::int64_t nlocal = v;
  std::int64_t global_offset_v = 0;
  MPI_Exscan(&nlocal, &global_offset_v, 1,
             dolfinx::MPI::mpi_type<std::int64_t>(), MPI_SUM, comm);

  // Communicate new global vertex index to neighbors
  auto [neighbor_comm, global_to_neighbor_rank]
      = compute_neighbor_comm(comm, mpi_rank, global_vertex_to_ranks);

  // Receive list of triplets mapping (input vertex index) -> (new global index,
  // owner)
  auto recv_triplets = send_vertex_numbering(
      neighbor_comm, global_to_neighbor_rank, mpi_rank, global_vertex_to_ranks,
      global_offset_v, global_to_local_vertices);
  assert(recv_triplets.size() % 3 == 0);

  // Unpack received data and make list of ghosts
  std::vector<std::int64_t> ghost_vertices;
  std::vector<int> ghost_vertex_owners;
  for (std::size_t i = 0; i < recv_triplets.size(); i += 3)
  {
    const std::int64_t gi = recv_triplets[i];
    const auto it = global_to_local_vertices.find(gi);
    assert(it != global_to_local_vertices.end());
    assert(it->second == -1);
    it->second = v++;
    ghost_vertices.push_back(recv_triplets[i + 1]);
    ghost_vertex_owners.push_back(recv_triplets[i + 2]);
  }

  if (ghost_mode != mesh::GhostMode::none)
  {
    // Receive index of ghost vertices that are not on the process
    // boundary from the ghost cell owner. Note: the ghost cell owner
    // might not be the same as the vertex owner.
    const std::vector<std::int64_t> recv_triplets = send_ghost_vertex_numbering(
        neighbor_comm, mpi_rank, global_to_neighbor_rank, index_map_c, cells,
        nlocal, global_offset_v, global_to_local_vertices, ghost_vertices,
        ghost_vertex_owners);

    // Unpack received data and add to ghosts
    for (std::size_t i = 0; i < recv_triplets.size(); i += 3)
    {
      const std::int64_t gi = recv_triplets[i];
      const auto it = global_to_local_vertices.find(gi);
      assert(it != global_to_local_vertices.end());
      if (it->second == -1)
      {
        it->second = v++;
        ghost_vertices.push_back(recv_triplets[i + 1]);
        ghost_vertex_owners.push_back(recv_triplets[i + 2]);
      }
    }
  }

  MPI_Comm_free(&neighbor_comm);

  // Convert input cell topology to local vertex indexing
  std::shared_ptr<graph::AdjacencyList<std::int32_t>> local_indexed_cells
      = std::make_shared<graph::AdjacencyList<std::int32_t>>(
          convert_cells_to_local_indexing(ghost_mode, cells, num_local_cells,
                                          global_to_local_vertices));

  Topology topology(comm, cell_type);
  const int tdim = topology.dim();

  // Create vertex index map
  auto index_map_v = std::make_shared<common::IndexMap>(
      comm, nlocal,
      dolfinx::MPI::compute_graph_edges(
          comm, std::set<int>(ghost_vertex_owners.begin(),
                              ghost_vertex_owners.end())),
      ghost_vertices, ghost_vertex_owners);
  topology.set_index_map(0, index_map_v);
  auto c0 = std::make_shared<graph::AdjacencyList<std::int32_t>>(
      index_map_v->size_local() + index_map_v->num_ghosts());
  topology.set_connectivity(c0, 0, 0);

  // Cell IndexMap
  topology.set_index_map(tdim, index_map_c);
  topology.set_connectivity(local_indexed_cells, tdim, 0);

  return topology;
}
//-----------------------------------------------------------------------------
