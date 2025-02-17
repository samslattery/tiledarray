/*
 *  This file is a part of TiledArray.
 *  Copyright (C) 2013  Virginia Tech
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <random>
#include <chrono>
#include <cstdio>
#include <unistd.h>

#include <madness/world/text_fstream_archive.h>
#include <madness/world/binary_fstream_archive.h>

#include <array_fixture.h>
#include "tiledarray.h"
#include "unit_test_config.h"
#include "../src/TiledArray/dist_array.h"

using namespace TiledArray;

ArrayFixture::ArrayFixture() :
    shape_tensor(tr.tiles_range(), 0.0),
    world(*GlobalFixture::world),
    a(world, tr)
{
  for(ArrayN::range_type::const_iterator it = a.range().begin(); it != a.range().end(); ++it)
    if(a.is_local(*it))
      a.set(*it, world.rank() + 1); // Fill the tile at *it (the index)

  for(std::size_t i = 0; i < tr.tiles_range().volume(); ++i) {
    if (i % 3) shape_tensor[i] = 1.0;
  }

  b = decltype(b)(world, tr, TiledArray::SparseShape<float>(shape_tensor, tr));
  for(SpArrayN::range_type::const_iterator it = b.range().begin(); it != b.range().end(); ++it)
    if(!b.is_zero(*it) && b.is_local(*it))
      b.set(*it, world.rank() + 1); // Fill the tile at *it (the index)

  world.gop.fence();
}

ArrayFixture::~ArrayFixture() {
  GlobalFixture::world->gop.fence();
}


BOOST_FIXTURE_TEST_SUITE( array_suite , ArrayFixture )

BOOST_AUTO_TEST_CASE( constructors )
{
  // Construct a dense array
  BOOST_REQUIRE_NO_THROW(ArrayN ad(world, tr));
  ArrayN ad(world, tr);

  // Check that none of the tiles have been set.
  for(ArrayN::const_iterator it = ad.begin(); it != ad.end(); ++it)
    BOOST_CHECK(! it->probe());

  // Construct a sparse array
  BOOST_REQUIRE_NO_THROW(SpArrayN as(world, tr, TiledArray::SparseShape<float>(shape_tensor, tr)));
  SpArrayN as(world, tr, TiledArray::SparseShape<float>(shape_tensor, tr));

  // Check that none of the tiles have been set.
  for(SpArrayN::const_iterator it = as.begin(); it != as.end(); ++it)
    BOOST_CHECK(! it->probe());

}

BOOST_AUTO_TEST_CASE( all_owned )
{
  std::size_t count = 0ul;
  for(std::size_t i = 0ul; i < tr.tiles_range().volume(); ++i)
    if(a.owner(i) == GlobalFixture::world->rank())
      ++count;
  world.gop.sum(count);

  // Check that all tiles are in the array
  BOOST_CHECK_EQUAL(tr.tiles_range().volume(), count);
}

BOOST_AUTO_TEST_CASE( owner )
{
  // Test to make sure everyone agrees who owns which tiles.
  std::shared_ptr<ProcessID> group_owner(new ProcessID[world.size()],
      std::default_delete<ProcessID[]>());

  size_type o = 0;
  for(ArrayN::range_type::const_iterator it = a.range().begin(); it != a.range().end(); ++it, ++o) {
    // Check that local ownership agrees
    const int owner = a.owner(*it);
    BOOST_CHECK_EQUAL(a.owner(o), owner);

    // Get the owner from all other processes
    int count = (owner == world.rank() ? 1 : 0);
    world.gop.sum(count);
    // Check that only one node claims ownership
    BOOST_CHECK_EQUAL(count, 1);

    std::fill_n(group_owner.get(), world.size(), 0);
    group_owner.get()[world.rank()] = owner;
    world.gop.reduce(group_owner.get(), world.size(), std::plus<ProcessID>());


    // Check that everyone agrees who the owner of the tile is.
    BOOST_CHECK((std::find_if(group_owner.get(), group_owner.get() + world.size(),
        std::bind(std::not_equal_to<ProcessID>(), owner, std::placeholders::_1)) == (group_owner.get() + world.size())));

  }
}

BOOST_AUTO_TEST_CASE( is_local )
{
  // Test to make sure everyone agrees who owns which tiles.

  size_type o = 0;
  for(ArrayN::range_type::const_iterator it = a.range().begin(); it != a.range().end(); ++it, ++o) {
    // Check that local ownership agrees
    const bool local_tile = a.owner(o) == world.rank();
    BOOST_CHECK_EQUAL(a.is_local(*it), local_tile);
    BOOST_CHECK_EQUAL(a.is_local(o), local_tile);

    // Find out how many claim ownership
    int count = (local_tile ? 1 : 0);
    world.gop.sum(count);

    // Check how many process claim ownership
    // "There can be only one!"
    BOOST_CHECK_EQUAL(count, 1);
  }
}

BOOST_AUTO_TEST_CASE( find_local )
{
  for(ArrayN::range_type::const_iterator it = a.range().begin(); it != a.range().end(); ++it) {

    if(a.is_local(*it)) {
      Future<ArrayN::value_type> tile = a.find(*it);

      BOOST_CHECK(tile.probe());

      const int value = world.rank() + 1;
      for(ArrayN::value_type::iterator it = tile.get().begin(); it != tile.get().end(); ++it)
        BOOST_CHECK_EQUAL(*it, value);
    }
  }
}

BOOST_AUTO_TEST_CASE( find_remote )
{
  for(ArrayN::range_type::const_iterator it = a.range().begin(); it != a.range().end(); ++it) {

    if(! a.is_local(*it)) {
      Future<ArrayN::value_type> tile = a.find(*it);

      const int owner = a.owner(*it);
      for(ArrayN::value_type::iterator it = tile.get().begin(); it != tile.get().end(); ++it)
        BOOST_CHECK_EQUAL(*it, owner + 1);
    }
  }
}

BOOST_AUTO_TEST_CASE( fill_tiles )
{
  ArrayN a(world, tr);

  for(ArrayN::range_type::const_iterator it = a.range().begin(); it != a.range().end(); ++it) {
    if(a.is_local(*it)) {
      a.set(*it, 0); // Fill the tile at *it (the index) with 0

      Future<ArrayN::value_type> tile = a.find(*it);

      // Check that the range for the constructed tile is correct.
      BOOST_CHECK_EQUAL(tile.get().range(), tr.make_tile_range(*it));

      for(ArrayN::value_type::iterator it = tile.get().begin(); it != tile.get().end(); ++it)
        BOOST_CHECK_EQUAL(*it, 0);
    }
  }
}

BOOST_AUTO_TEST_CASE( assign_tiles )
{
  std::vector<int> data;
  ArrayN a(world, tr);

  for(ArrayN::range_type::const_iterator it = a.range().begin(); it != a.range().end(); ++it) {
    ArrayN::trange_type::range_type range = a.trange().make_tile_range(*it);
    if(a.is_local(*it)) {
      if(data.size() < range.volume())
        data.resize(range.volume(), 1);
      a.set(*it, data.begin());

      Future<ArrayN::value_type> tile = a.find(*it);
      BOOST_CHECK(tile.probe());

      // Check that the range for the constructed tile is correct.
      BOOST_CHECK_EQUAL(tile.get().range(), tr.make_tile_range(*it));

      for(ArrayN::value_type::iterator it = tile.get().begin(); it != tile.get().end(); ++it)
        BOOST_CHECK_EQUAL(*it, 1);
    }
  }
}

BOOST_AUTO_TEST_CASE( clone )
{
  std::vector<int> data;
  ArrayN a(world, tr);

  // Init tiles with random data
  a.init_tiles([](const Range& range) -> TensorI {
    std::default_random_engine generator(std::chrono::system_clock::now().time_since_epoch().count());
    std::uniform_int_distribution<int> distribution(0,100);

    TensorI tile(range);
    tile.inplace_unary([&] (int& value) { value = distribution(generator); });
    return tile;
  });

  ArrayN ca;
  BOOST_REQUIRE_NO_THROW(ca = TiledArray::clone(a));

  // Check array data are equal
  BOOST_CHECK(!(ca.id() == a.id()));
  BOOST_CHECK_EQUAL(ca.world().id(), a.world().id());
  BOOST_CHECK_EQUAL(ca.trange(), a.trange());
  BOOST_CHECK_EQUAL_COLLECTIONS(ca.pmap()->begin(), ca.pmap()->end(),
      a.pmap()->begin(), a.pmap()->end());

  // Check that array tiles are equal
  for(typename ArrayN::size_type index = 0ul; index < a.size(); ++index) {
    // Check that per-tile information is the same
    BOOST_CHECK_EQUAL(ca.owner(index), a.owner(index));
    BOOST_CHECK_EQUAL(ca.is_local(index), a.is_local(index));
    BOOST_CHECK_EQUAL(ca.is_zero(index), a.is_zero(index));

    // Skip non-local tiles
    if(! a.is_local(index))
      continue;

    const TensorI t = a.find(index).get();
    const TensorI ct = ca.find(index).get();

    // Check that tile data is the same but held in different memory locations
    BOOST_CHECK_NE(ct.data(), t.data());
    BOOST_CHECK_EQUAL(ct.range(), t.range());
    BOOST_CHECK_EQUAL_COLLECTIONS(ct.begin(), ct.end(), t.begin(), t.end());

  }
}

BOOST_AUTO_TEST_CASE( make_replicated )
{
  // Get a copy of the original process map
  std::shared_ptr<ArrayN::pmap_interface> distributed_pmap = a.pmap();

  // Convert array to a replicated array.
  BOOST_REQUIRE_NO_THROW(a.make_replicated());

  if(GlobalFixture::world->size() == 1)
    BOOST_CHECK(! a.pmap()->is_replicated());
  else
    BOOST_CHECK(a.pmap()->is_replicated());

  // Check that all the data is local
  for(std::size_t i = 0; i < a.size(); ++i) {
    BOOST_CHECK(a.is_local(i));
    BOOST_CHECK_EQUAL(a.pmap()->owner(i), GlobalFixture::world->rank());
    Future<ArrayN::value_type> tile = a.find(i);
    BOOST_CHECK_EQUAL(tile.get().range(), a.trange().make_tile_range(i));
    for(ArrayN::value_type::const_iterator it = tile.get().begin(); it != tile.get().end(); ++it)
      BOOST_CHECK_EQUAL(*it, distributed_pmap->owner(i) + 1);
  }
}

BOOST_AUTO_TEST_CASE( serialization_by_tile )
{
  decltype(a) acopy(a.world(), a.trange(), a.shape());

  const auto nproc = world.size();
  if (nproc > 1) {  // use BufferOutputArchive if more than 1 node ...
    const std::size_t buf_size = 1000000;  // "big enough" buffer
    auto buf = std::make_unique<unsigned char[]>(buf_size);
    madness::archive::BufferOutputArchive oar(buf.get(), buf_size);

    for (auto tile : a) {
      BOOST_REQUIRE_NO_THROW(oar & tile.get());
    }

    std::size_t nbyte = oar.size();
    BOOST_REQUIRE(nbyte < buf_size);
    oar.close();

    madness::archive::BufferInputArchive iar(buf.get(), nbyte);
    for (auto tile : acopy) {
      decltype(acopy)::value_type tile_value;
      BOOST_REQUIRE_NO_THROW(iar & tile_value);
      tile.future().set(std::move(tile_value));
    }
    iar.close();

    buf.reset();
  }
  else {  // ... else use TextFstreamOutputArchive
    char archive_file_name[] = "tmp.XXXXXX";
    mktemp(archive_file_name);
    madness::archive::TextFstreamOutputArchive oar(archive_file_name);

    for(auto tile : a) {
      BOOST_REQUIRE_NO_THROW(oar & tile.get());
    }

    oar.close();

    madness::archive::TextFstreamInputArchive iar(archive_file_name);
    for(auto tile : acopy) {
      decltype(acopy)::value_type tile_value;
      BOOST_REQUIRE_NO_THROW(iar & tile_value);
      tile.future().set(std::move(tile_value));
    }
    iar.close();

    std::remove(archive_file_name);
  }

  BOOST_CHECK_EQUAL(a.trange(), acopy.trange());
  BOOST_REQUIRE(a.shape() == acopy.shape());
  BOOST_CHECK_EQUAL_COLLECTIONS(a.begin(), a.end(), acopy.begin(), acopy.end());
}

BOOST_AUTO_TEST_CASE( dense_serialization )
{
  char archive_file_name[] = "tmp.XXXXXX";
  mktemp(archive_file_name);
  madness::archive::BinaryFstreamOutputArchive oar(archive_file_name);
  a.serialize(oar);

  oar.close();

  madness::archive::BinaryFstreamInputArchive iar(archive_file_name);
  decltype(a) aread;
  aread.serialize(iar);

  BOOST_CHECK_EQUAL(aread.trange(), a.trange());
  BOOST_REQUIRE(aread.shape() == a.shape());
  BOOST_CHECK_EQUAL_COLLECTIONS(aread.begin(), aread.end(), a.begin(), a.end());
}

BOOST_AUTO_TEST_CASE( sparse_serialization )
{
  char archive_file_name[] = "tmp.XXXXXX";
  mktemp(archive_file_name);
  madness::archive::BinaryFstreamOutputArchive oar(archive_file_name);
  b.serialize(oar);

  oar.close();

  madness::archive::BinaryFstreamInputArchive iar(archive_file_name);
  decltype(b) bread;
  bread.serialize(iar);

  BOOST_CHECK_EQUAL(bread.trange(), b.trange());
  BOOST_REQUIRE(bread.shape() == b.shape());
  BOOST_CHECK_EQUAL_COLLECTIONS(bread.begin(), bread.end(), b.begin(), b.end());
}

BOOST_AUTO_TEST_CASE( parallel_serialization )
{
  const int nio = 1; // use 1 rank for 1
  char archive_file_name[] = "tmp.XXXXXX";
  mktemp(archive_file_name);
  madness::archive::ParallelOutputArchive oar(world, archive_file_name, nio);
  oar & a;
  oar.close();

  madness::archive::ParallelInputArchive iar(world, archive_file_name, nio);
  decltype(a) aread;
  aread.load(world, iar);

  BOOST_CHECK_EQUAL(aread.trange(), a.trange());
  BOOST_REQUIRE(aread.shape() == a.shape());
  BOOST_CHECK_EQUAL_COLLECTIONS(aread.begin(), aread.end(), a.begin(), a.end());
}

BOOST_AUTO_TEST_CASE( parallel_sparse_serialization )
{
  const int nio = 1; // use 1 rank for 1
  char archive_file_name[] = "tmp.XXXXXX";
  mktemp(archive_file_name);
  madness::archive::ParallelOutputArchive oar(world, archive_file_name, nio);
  oar & b;
  oar.close();

  madness::archive::ParallelInputArchive iar(world, archive_file_name, nio);
  decltype(b) bread;
  bread.load(world, iar);

  BOOST_CHECK_EQUAL(bread.trange(), b.trange());
  BOOST_REQUIRE(bread.shape() == b.shape());
  BOOST_CHECK_EQUAL_COLLECTIONS(bread.begin(), bread.end(), b.begin(), b.end());
}

BOOST_AUTO_TEST_SUITE_END()

