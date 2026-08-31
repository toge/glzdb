#pragma once

#include <array>
#include <concepts>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include <glaze/core/meta.hpp>

namespace glzdb {
namespace detail {

  /// @brief 完全修飾型名から単純型名を抽出 (最後の ':' 以降)
  /// @param full 完全修飾型名
  /// @return 最後の ':' 以降の部分
  constexpr std::string_view simple_type_name(const std::string_view full) noexcept {
    if (const auto pos = full.find_last_of(':'); pos != std::string_view::npos) {
      return full.substr(pos + 1);
    }
    return full;
  }

}  // namespace detail

/// @brief モデルが `id` メンバを公開していることを要求する concept
///
/// `id` は主キー。copyable である必要がある (std::map のキーになるため)。
/// glaze の構造体リフレクションのためにはデフォルト構築可能でも必要。
template <class T>
concept has_id_member = requires(T t) {
  t.id;
  requires std::copyable<decltype(t.id)>;
};

/// @brief モデルの主キー型を取得 (decltype(T::id))
/// @tparam T モデル型
template <class T>
using id_type = decltype(std::declval<T&>().id);

/// @brief メンバポインタの所有者型を取得
/// @details member_owner_t<&T::member> == T
///          部分特殊化 M T::* -> T により実現
template <auto Member>
struct member_owner_t_ {};
template <class T, class M, M T::* Member>
struct member_owner_t_<Member> {
  using type = T;
};

/// @brief メンバポインタの値型を取得
/// @details member_value_t<&T::member> == M
template <auto Member>
struct member_value_t_ {};
template <class T, class M, M T::* Member>
struct member_value_t_<Member> {
  using type = M;
};

template <auto Member>
using member_owner_t = typename member_owner_t_<Member>::type;

template <auto Member>
using member_value_t = typename member_value_t_<Member>::type;

/// @brief モデル型の名称を取得 (コンパイル時定数)
/// @details
/// 既定値は glz::name_v<T>。ユーザーが glz::meta<T>::name を特殊化した場合は
/// その値が使用される。いずれにせよ返される string_view はコンパイル時定数。
///
/// glz::name_v<T> は ( namespace 修飾された) 型名にフォールバックし、
/// それを detail::simple_type_name() で最後の ':' 以降に切り出して
/// テーブルファイル名を予測可能にする。
/// @warning 異なる namespace に同名の型があると単純名が衝突し、同一 `<name>.json`
///          に上書きされる。`JsonPartitionedAdapter` 使用時は `glz::meta<T>` で
///          `name` を上書きするか、型名を別にすること。衝突はコンパイル時に
///          `static_assert` で検出される。
/// @tparam T モデル型
template <class T>
consteval auto name_of() {
  constexpr std::string_view full = glz::name_v<std::remove_cvref_t<T>>;
  return detail::simple_type_name(full);
}

/// @brief name_of<T>() の constexpr キャッシング版
template <class T>
constexpr std::string_view name_of_v = name_of<T>();

namespace detail {
  template <class State, std::size_t... Is>
  constexpr bool has_duplicate_impl(std::index_sequence<Is...>) {
    constexpr std::array<std::string_view, sizeof...(Is)> names{
        name_of_v<typename std::tuple_element_t<Is, State>::value_type>...};
    for (std::size_t i = 0; i < names.size(); ++i) {
      for (std::size_t j = i + 1; j < names.size(); ++j) {
        if (names[i] == names[j]) {
          return true;
        }
      }
    }
    return false;
  }
}  // namespace detail

/// @brief Partitioned 用にテーブル名の重複を検出
/// @details State タプル内の全テーブルで name_of_v が重複していないか判定
/// @tparam State テーブルタプル型
template <class State>
constexpr bool has_duplicate_table_names() {
  constexpr std::size_t N = std::tuple_size_v<State>;
  if constexpr (N <= 1) {
    return false;
  } else {
    return detail::has_duplicate_impl<State>(std::make_index_sequence<N>{});
  }
}

}  // namespace glzdb
