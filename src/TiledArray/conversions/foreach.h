/*
 *  This file is a part of TiledArray.
 *  Copyright (C) 2015  Virginia Tech
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
 *  Justus Calvin
 *  Department of Chemistry, Virginia Tech
 *
 *  foreach.h
 *  Apr 15, 2015
 *
 */

#ifndef TILEDARRAY_CONVERSIONS_FOREACH_H__INCLUDED
#define TILEDARRAY_CONVERSIONS_FOREACH_H__INCLUDED

#include <TiledArray/type_traits.h>

/// Forward declarations
namespace Eigen {
  template <typename> class aligned_allocator;
} // namespace Eigen

namespace TiledArray {

  /// Forward declarations
  template <typename, typename> class DistArray;
  template <typename, typename> class Tensor;
  class DensePolicy;
  class SparsePolicy;

  enum class ShapeReductionMethod {Union, Intersect};

  namespace detail {

    namespace {

      template <bool inplace, typename Result = void>
      struct void_op_helper;

      template <typename Result>
      struct void_op_helper<false, Result> {
        template <typename Op, typename Arg, typename... Args>
        Result operator()(Op&&op, Arg&& arg, Args&&... args) {
          Result result;
          op(result, std::forward<Arg>(arg), std::forward<Args>(args)...);
          return result;
        }
      };
      template <typename Result>
      struct void_op_helper<true, Result> {
        template <typename Op, typename Arg, typename... Args>
        decltype(auto) operator()(Op&&op, Arg&& arg, Args&&... args) {
          op(std::forward<Arg>(arg), std::forward<Args>(args)...);
          return arg;
        }
      };

      template <bool inplace, typename Result = void>
      struct nonvoid_op_helper;

      template <typename Result>
      struct nonvoid_op_helper<false, Result> {
        template <typename Op, typename OpResult,
                  typename Arg, typename... Args>
        Result operator()(Op&&op, OpResult& op_result,
            Arg&& arg, Args&&... args) {
          Result result;
          op_result = op(result, std::forward<Arg>(arg), std::forward<Args>(args)...);
          return result;
        }
      };
      template <typename Result>
      struct nonvoid_op_helper<true, Result> {
        template <typename Op, typename OpResult,
                  typename Arg, typename... Args>
        std::decay_t<Arg> operator()(Op&&op, OpResult& op_result,
            Arg&& arg, Args&&... args) {
          op_result = op(std::forward<Arg>(arg), std::forward<Args>(args)...);
          return arg;
        }
      };

      template <typename Tile, typename Policy>
      inline bool compare_trange(const DistArray<Tile, Policy>& array1) {
        return true;
      }

      template <typename Tile1, typename Tile2, typename Policy, typename... Arrays>
      inline bool compare_trange(const DistArray<Tile1, Policy>& array1,
          const DistArray<Tile2, Policy>& array2, const Arrays&... arrays) {
        return(array1.trange() == array2.trange()
            && compare_trange(array1, arrays...));
      }

      inline bool is_zero_intersection(const std::initializer_list<bool>& is_zero_list ) {
        return std::any_of(is_zero_list.begin(), is_zero_list.end(),
            [](const bool val) -> bool {return val;});
      }
      inline bool is_zero_union(const std::initializer_list<bool>& is_zero_list ) {
        return std::all_of(is_zero_list.begin(), is_zero_list.end(),
            [](const bool val) -> bool {return val;});
      }

      template <typename I, typename A>
      Future<typename A::value_type> get_sparse_tile(const I& index, const A& array) {
        return (!array.is_zero(index)? array.find(index)
            : Future<typename A::value_type>(typename A::value_type()));
      }
      template <typename I, typename A>
      Future<typename A::value_type> get_sparse_tile(const I& index, A& array) {
        return (!array.is_zero(index)? array.find(index)
            : Future<typename A::value_type>(typename A::value_type()));
      }

    }

    /// base implementation of dense TiledArray::foreach

    /// \note can't autodeduce \c ResultTile from \c void \c Op(ResultTile,ArgTile)
    template <bool inplace = false, typename Op,
        typename ResultTile, typename ArgTile, typename... ArgTiles>
    inline DistArray<ResultTile, DensePolicy> foreach ( Op && op,
        const_if_t<not inplace, DistArray<ArgTile, DensePolicy>>& arg,
        const DistArray<ArgTiles, DensePolicy>&... args) {

      TA_USER_ASSERT(compare_trange(arg, args...), "Tiled ranges of args must match");

      typedef DistArray<ArgTile, DensePolicy> arg_array_type;
      typedef DistArray<ResultTile, DensePolicy> result_array_type;

      World& world = arg.world();

      // Make an empty result array
      result_array_type result(world, arg.trange(), arg.pmap());

      // Construct the task function for making result tiles.
      auto task = [&op](const_if_t<not inplace, typename arg_array_type::value_type>& arg_tile,
          const ArgTiles&... arg_tiles) {
        void_op_helper<inplace, typename result_array_type::value_type> op_caller;
        return op_caller(std::forward<Op>(op), arg_tile, arg_tiles...);
      };

      // Iterate over local tiles of arg
      for (auto index: *(arg.pmap())) {
        // Spawn a task to evaluate the tile
        Future<typename result_array_type::value_type> tile =
            world.taskq.add(task, arg.find(index), args.find(index)...);

        // Store result tile
        result.set(index, tile);
      }

      return result;
    }

    /// base implementation of sparse TiledArray::foreach

    /// \note can't autodeduce \c ResultTile from \c void \c Op(ResultTile,ArgTile)
    template <bool inplace = false, typename Op,
        typename ResultTile, typename ArgTile, typename... ArgTiles>
    inline DistArray<ResultTile, SparsePolicy> foreach (Op&& op, const ShapeReductionMethod shape_reduction,
        const_if_t<not inplace, DistArray<ArgTile, SparsePolicy>>& arg,
        const DistArray<ArgTiles, SparsePolicy>&... args) {

      TA_USER_ASSERT(detail::compare_trange(arg, args...), "Tiled ranges of args must match");

      typedef DistArray<ArgTile, SparsePolicy> arg_array_type;
      typedef DistArray<ResultTile, SparsePolicy> result_array_type;

      typedef typename arg_array_type::value_type arg_value_type;
      typedef typename result_array_type::value_type result_value_type;
      typedef typename arg_array_type::size_type size_type;
      typedef typename arg_array_type::shape_type shape_type;
      typedef std::pair<size_type, Future<result_value_type>> datum_type;

      // Create a vector to hold local tiles
      std::vector<datum_type> tiles;
      tiles.reserve(arg.pmap()->size());

      // Construct a tensor to hold updated tile norms for the result shape.
      TiledArray::Tensor<typename shape_type::value_type,
          Eigen::aligned_allocator<typename shape_type::value_type> >
      tile_norms(arg.trange().tiles_range(), 0);

      // Construct the task function used to construct the result tiles.
      madness::AtomicInt counter; counter = 0;
      int task_count = 0;
      auto task = [&op,&counter,&tile_norms](const size_type index,
          const_if_t<not inplace, arg_value_type>& arg_tile,
          const ArgTiles&... arg_tiles) -> result_value_type {
        nonvoid_op_helper<inplace, result_value_type> op_caller;
        auto result_tile = op_caller(std::forward<Op>(op), tile_norms[index],
            arg_tile, arg_tiles...);
        ++counter;
        return result_tile;
      };

      World& world = arg.world();

      switch (shape_reduction) {
      case ShapeReductionMethod::Intersect:
        // Get local tile index iterator
        for(auto index: *(arg.pmap())) {
          if(is_zero_intersection({arg.is_zero(index), args.is_zero(index)...}))
            continue;
          auto result_tile = world.taskq.add(task, index, arg.find(index),
              args.find(index)...);
          ++task_count;
          tiles.emplace_back(index, std::move(result_tile));
        }
        break;
      case ShapeReductionMethod::Union:
        // Get local tile index iterator
        for(auto index: *(arg.pmap())) {
          if(is_zero_union({arg.is_zero(index), args.is_zero(index)...}))
            continue;
          auto result_tile = world.taskq.add(task, index, detail::get_sparse_tile(index, arg),
              detail::get_sparse_tile(index, args)...);
          ++task_count;
          tiles.emplace_back(index, std::move(result_tile));
        }
        break;
      default:
        TA_ASSERT(false);
        break;
      }

      // Wait for tile norm data to be collected.
      if(task_count > 0)
        world.await([&counter,task_count] () -> bool { return counter == task_count; });

      // Construct the new array
      result_array_type result(world, arg.trange(),
          shape_type(world, tile_norms, arg.trange()), arg.pmap());
      for(typename std::vector<datum_type>::const_iterator it = tiles.begin(); it != tiles.end(); ++it) {
        const size_type index = it->first;
        if(! result.is_zero(index))
          result.set(it->first, it->second);
      }

      return result;
    }

  } // namespace TiledArray::detail

  /// Apply a function to each tile of a dense Array

  /// This function uses an \c Array object to generate a new \c Array where the
  /// output tiles are a function of the input tiles. Users must provide a
  /// function/functor that initializes the tiles for the new \c Array object.
  /// For example, if we want to create a new array with were each element is
  /// equal to the square root of the corresponding element of the original
  /// array:
  /// \code
  /// TiledArray::Array<2, double> out_array =
  ///     foreach(in_array, [=] (TiledArray::Tensor<double>& out_tile,
  ///                            const TiledArray::Tensor<double>& in_tile) {
  ///       out_tile = in_tile.unary([=] (const double value) -> double
  ///           { return std::sqrt(value); });
  ///     });
  /// \endcode
  /// The expected signature of the tile operation is:
  /// \code
  /// void op(      typename TiledArray::DistArray<ResultTile,DensePolicy>::value_type& result_tile,
  ///         const typename TiledArray::DistArray<ArgTile,DensePolicy>::value_type& arg_tile);
  /// \endcode
  /// \tparam Op Tile operation
  /// \tparam ResultTile The tile type of the result array
  /// \tparam ArgTile The tile type of \c arg
  /// \param op The tile function
  /// \param arg The argument array
  template <typename ResultTile, typename ArgTile, typename Op,
            typename = typename std::enable_if<!std::is_same<ResultTile,ArgTile>::value>::type>
  inline DistArray<ResultTile, DensePolicy>
  foreach(const DistArray<ArgTile, DensePolicy>& arg, Op&& op) {
    return detail::foreach<false, Op, ResultTile, ArgTile>(std::forward<Op>(op), arg);
  }

  /// Apply a function to each tile of a dense Array

  /// Specialization of foreach<ResultTile,ArgTile,Op> for
  /// the case \c ResultTile == \c ArgTile
  template <typename Tile, typename Op>
  inline DistArray<Tile, DensePolicy>
  foreach(const DistArray<Tile, DensePolicy>& arg, Op&& op) {
    return detail::foreach<false, Op, Tile, Tile>(std::forward<Op>(op), arg);
  }

  /// Modify each tile of a dense Array

  /// This function modifies the tile data of \c Array object. Users must
  /// provide a function/functor that modifies the tile data. For example, if we
  /// want to modify the elements of the array to be equal to the square
  /// root of the original value:
  /// \code
  /// foreach(array, [] (TiledArray::TensorD& tile) {
  ///   tile.inplace_unary([&] (double& value) { value = std::sqrt(value); });
  /// });
  /// \endcode
  /// The expected signature of the tile operation is:
  /// \code
  /// void op(typename TiledArray::DistArray<Tile,DensePolicy>::value_type& tile);
  /// \endcode
  /// \tparam Op Mutating tile operation
  /// \tparam Tile The tile type of the array
  /// \param op The mutating tile function
  /// \param arg The argument array to be modified
  /// \param fence A flag that indicates fencing behavior. If \c true this
  /// function will fence before data is modified.
  /// \warning This function fences by default to avoid data race conditions.
  /// Only disable the fence if you can ensure, the data is not being read by
  /// another thread.
  /// \warning If there is a another copy of \c arg that was created via (or
  /// arg was created by) the \c Array copy constructor or copy assignment
  /// operator, this function will modify the data of that array since the data
  /// of a tile is held in a \c std::shared_ptr. If you need to ensure other
  /// copies of the data are not modified or this behavior causes problems in
  /// your application, use the \c TiledArray::foreach function instead.
  template <typename Tile, typename Op,
      typename = typename std::enable_if<! TiledArray::detail::is_array<typename std::decay<Op>::type>::value>::type>
  inline void
  foreach_inplace(DistArray<Tile, DensePolicy>& arg, Op&& op, bool fence = true) {
    // The tile data is being modified in place, which means we may need to
    // fence to ensure no other threads are using the data.
    if(fence)
      arg.world().gop.fence();

    arg = detail::foreach<true, Op, Tile, Tile>(std::forward<Op>(op), arg);
  }

  /// Apply a function to each tile of a sparse Array

  /// This function uses an \c Array object to generate a new \c Array where the
  /// output tiles are a function of the input tiles. Users must provide a
  /// function/functor that initializes the tiles for the new \c Array object.
  /// For example, if we want to create a new array with were each element is
  /// equal to the square root of the corresponding element of the original
  /// array:
  /// \code
  /// TiledArray::Array<2, double, Tensor<double>, SparsePolicy> out_array =
  ///     foreach(in_array, [] (TiledArray::Tensor<double>& out_tile,
  ///                           const TiledArray::Tensor<double>& in_tile) -> float
  ///     {
  ///       double norm_squared = 0.0;
  ///       out_tile = in_tile.unary([&] (const double value) -> double {
  ///         const double result = std::sqrt(value);
  ///         norm_squared += result * result;
  ///         return result;
  ///       });
  ///       return std::sqrt(norm_squared);
  ///     });
  /// \endcode
  /// The expected signature of the tile operation is:
  /// \code
  /// float op(typename TiledArray::DistArray<Tile,SparsePolicy>::value_type& result_tile,
  ///     const typename TiledArray::DistArray<Tile,SparsePolicy>::value_type& arg_tile);
  /// \endcode
  /// where the return value of \c op is the 2-norm (Frobenius norm) of the
  /// result tile.
  /// \note This function should not be used to initialize the tiles of an array
  /// object.
  /// \tparam Op Tile operation
  /// \tparam Tile The tile type of the array
  /// \param op The tile function
  /// \param arg The argument array
  template <typename ResultTile, typename ArgTile, typename Op,
            typename = typename std::enable_if<!std::is_same<ResultTile,ArgTile>::value>::type>
  inline DistArray<ResultTile, SparsePolicy>
  foreach(const DistArray<ArgTile, SparsePolicy> arg, Op&& op) {
    return detail::foreach<false, Op, ResultTile, ArgTile>(std::forward<Op>(op), ShapeReductionMethod::Intersect, arg);
  }

  /// Apply a function to each tile of a sparse Array

  /// Specialization of foreach<ResultTile,ArgTile,Op> for
  /// the case \c ResultTile == \c ArgTile
  template <typename Tile, typename Op>
  inline DistArray<Tile, SparsePolicy>
  foreach(const DistArray<Tile, SparsePolicy>& arg, Op&& op) {
    return detail::foreach<false, Op, Tile, Tile>(std::forward<Op>(op), ShapeReductionMethod::Intersect, arg);
  }


  /// Modify each tile of a sparse Array

  /// This function modifies the tile data of \c Array object. Users must
  /// provide a function/functor that modifies the tile data in place. For
  /// example, if we want to modify the elements of the array to be equal to the
  /// square root of the original value:
  /// \code
  /// foreach(array, [] (TiledArray::Tensor<double>& tile) -> float {
  ///   double norm_squared = 0.0;
  ///   tile.inplace_unary([&] (double& value) {
  ///     norm_squared += value; // Assume value >= 0
  ///     value = std::sqrt(value);
  ///   });
  ///   return std::sqrt(norm_squared);
  /// });
  /// \endcode
  /// The expected signature of the tile operation is:
  /// \code
  /// float op(typename TiledArray::DistArray<Tile,SparsePolicy>::value_type& tile);
  /// \endcode
  /// where the return value of \c op is the 2-norm (Fibrinous norm) of the
  /// tile.
  /// \note This function should not be used to initialize the tiles of an array
  /// object.
  /// \tparam Op Tile operation
  /// \tparam Tile The tile type of the array
  /// \param op The mutating tile function
  /// \param arg The argument array to be modified
  /// \param fence A flag that indicates fencing behavior. If \c true this
  /// function will fence before data is modified.
  /// \warning This function fences by default to avoid data race conditions.
  /// Only disable the fence if you can ensure, the data is not being read by
  /// another thread.
  /// \warning If there is a another copy of \c arg that was created via (or
  /// arg was created by) the \c Array copy constructor or copy assignment
  /// operator, this function will modify the data of that array since the data
  /// of a tile is held in a \c std::shared_ptr. If you need to ensure other
  /// copies of the data are not modified or this behavior causes problems in
  /// your application, use the \c TiledArray::foreach function instead.
  template <typename Tile, typename Op,
      typename = typename std::enable_if<! TiledArray::detail::is_array<typename std::decay<Op>::type>::value>::type>
  inline void
  foreach_inplace(DistArray<Tile, SparsePolicy>& arg, Op&& op, bool fence = true) {

    // The tile data is being modified in place, which means we may need to
    // fence to ensure no other threads are using the data.
    if(fence)
      arg.world().gop.fence();

    // Set the arg with the new array
    arg = detail::foreach<true, Op, Tile, Tile>(std::forward<Op>(op), ShapeReductionMethod::Intersect, arg);
  }

  /// Apply a function to each tile of dense Arrays
  /// The following function takes two input tiles
  template <typename ResultTile, typename LeftTile, typename RightTile, typename Op,
            typename = typename std::enable_if<!std::is_same<ResultTile, LeftTile>::value>::type>
  inline DistArray<ResultTile, DensePolicy>
  foreach(const DistArray<LeftTile, DensePolicy>& left,
      const DistArray<RightTile, DensePolicy>& right, Op&& op) {
    return detail::foreach<false, Op, ResultTile, LeftTile, RightTile>(std::forward<Op>(op),
        left, right);
  }

  /// Specialization of foreach<ResultTile,ArgTile,Op> for
  /// the case \c ResultTile == \c ArgTile
  template <typename LeftTile, typename RightTile, typename Op>
  inline DistArray<LeftTile, DensePolicy>
  foreach(const DistArray<LeftTile, DensePolicy>& left,
      const DistArray<RightTile, DensePolicy>& right, Op&& op) {
    return detail::foreach<false, Op, LeftTile, LeftTile, RightTile>(std::forward<Op>(op),
        left, right);
  }

  /// This function takes two input tiles and put result into the left tile
  template <typename LeftTile, typename RightTile, typename Op>
  inline void
  foreach_inplace(DistArray<LeftTile, DensePolicy>& left,
      const DistArray<RightTile, DensePolicy>& right, Op&& op, bool fence = true) {
    // The tile data is being modified in place, which means we may need to
    // fence to ensure no other threads are using the data.
    if(fence)
      left.world().gop.fence();

    left = detail::foreach<true, Op, LeftTile, LeftTile, RightTile>(std::forward<Op>(op),
        left, right);
  }

  /// Apply a function to each tile of sparse Arrays
  /// The following function takes two input tiles
  template <typename ResultTile, typename LeftTile, typename RightTile, typename Op,
            typename = typename std::enable_if<!std::is_same<ResultTile, LeftTile>::value>::type>
  inline DistArray<ResultTile, SparsePolicy>
  foreach(const DistArray<LeftTile, SparsePolicy>& left,
      const DistArray<RightTile, SparsePolicy>& right, Op&& op,
      const ShapeReductionMethod shape_reduction = ShapeReductionMethod::Intersect) {
    return detail::foreach<false, Op, ResultTile, LeftTile, RightTile>(std::forward<Op>(op),
        shape_reduction, left, right);
  }

  /// Specialization of foreach<ResultTile,ArgTile,Op> for
  /// the case \c ResultTile == \c ArgTile
  template <typename LeftTile, typename RightTile, typename Op>
  inline DistArray<LeftTile, SparsePolicy>
  foreach(const DistArray<LeftTile, SparsePolicy>& left,
      const DistArray<RightTile, SparsePolicy>& right, Op&& op,
      const ShapeReductionMethod shape_reduction = ShapeReductionMethod::Intersect) {
    return detail::foreach<false, Op, LeftTile, LeftTile, RightTile>(std::forward<Op>(op),
        shape_reduction, left, right);
  }

  /// This function takes two input tiles and put result into the left tile
  template <typename LeftTile, typename RightTile, typename Op>
  inline void
  foreach_inplace(DistArray<LeftTile, SparsePolicy>& left,
      const DistArray<RightTile, SparsePolicy>& right, Op&& op,
      const ShapeReductionMethod shape_reduction = ShapeReductionMethod::Intersect,
      bool fence = true) {

    // The tile data is being modified in place, which means we may need to
    // fence to ensure no other threads are using the data.
    if(fence)
      left.world().gop.fence();

    // Set the arg with the new array
    left = detail::foreach<true, Op, LeftTile, LeftTile, RightTile>(std::forward<Op>(op),
        shape_reduction, left, right);
  }

} // namespace TiledArray

#endif // TILEDARRAY_CONVERSIONS_TRUNCATE_H__INCLUDED
