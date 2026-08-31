#pragma once

#include <map>

#include "traits.hpp"

namespace glzdb {

/// @brief テーブル — モデルの主キーから行への順序付きマップ
///
/// `rows` は public にしているため呼び出し側が直接反復・検査できるが、
/// 推奨インターフェースは database の CRUD 操作である。
///
/// @tparam T モデル型 (has_id_member concept を満たす)
///
/// @note std::map (ordered) を使用。unordered_map への切替は行わない。
template <class T>
  requires has_id_member<T>
struct table {
  using value_type = T;            ///< モデル型
  using id_type    = glzdb::id_type<T>;  ///< 主キー型
  using map_type   = std::map<id_type, T>;  ///< 内部ストレージ

  map_type rows;   ///< id -> 行のマップ
};

}  // namespace glzdb
