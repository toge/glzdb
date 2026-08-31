#pragma once

#include <filesystem>
#include <fstream>
#include <string>
#include <tuple>

#include <glaze/core/context.hpp>
#include <glaze/json/read.hpp>
#include <glaze/json/write.hpp>

#include "error.hpp"
#include "table.hpp"
#include "traits.hpp"

namespace glzdb {
namespace detail {

  /// @brief ファイル `path` の内容を `value` へ読み込む
  /// @details ファイルが存在しない場合は false を返す (呼び出し側が判断)。
  ///          ファイルが存在するが解析に失敗した場合は parse_error を返す。
  /// @tparam T 読み込み先の型
  /// @param value 読み込み先オブジェクト
  /// @param path ファイルパス
  /// @return 読み込み結果 (true=新規/成功, false=ファイル不存在, error=解析失敗)
  template <class T>
  result<bool> read_file_into(T& value, const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
      return false;
    }
    std::string buffer;
    const auto  ec = glz::read_file_json(value, path.string(), buffer);
    if (bool(ec)) {
      return std::unexpected(error::parse_error);
    }
    return true;
  }

  /// @brief 値をファイルへ atomic に書き込む
  /// @details 一時ファイルへシリアライズして書き込み、同一ファイルシステム上での
  ///          rename() によりターゲットを置換する。rename は原子的操作なので、
  ///          リーダーは決して不完全なファイルを観測しない。
  /// @tparam T 書き込み対象の型
  /// @param value シリアライズする値
  /// @param path 書き込み先パス
  /// @return 成功時は none、失敗時はエラーコード
  template <class T>
  error write_atomic(const T& value, const std::filesystem::path& path) {
    std::string buffer;
    const auto  ec = glz::write_json(value, buffer);
    if (bool(ec)) {
      return error::parse_error;
    }

    auto tmp = path;
    tmp += ".tmp";

    std::error_code sys;
    if (std::ofstream out(tmp, std::ios::binary | std::ios::trunc); out) {
      out.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
      out.close();
      if (!out) {
        std::filesystem::remove(tmp, sys);
        return error::io_error;
      }
    } else {
      return error::io_error;
    }

    std::filesystem::rename(tmp, path, sys);
    if (sys) {
      std::filesystem::remove(tmp, sys);
      return error::io_error;
    }
    return {};
  }

}  // namespace detail

/// @defgroup adapters 永続化アダプタ
/// @brief データベースの永続化形式を定義する
///
/// アダプタは database<State, Adapter> の永続化を所有する。
/// 全ての書き込みは atomic (一時ファイル + rename) で行われる。
/// load() は新しい State を返す。解析できない既存ファイルはエラー、
/// ファイルが不存在する場合は「空で開始」を意味する。
///
/// @details
/// - JsonAdapter: 全 State を 1 つの JSON ファイルに出力
/// - JsonPartitionedAdapter: テーブル毎に 1 ファイル、<name_of<T>>.json として出力

/// @brief アダプタ concept: load/save 静的メソッドを提供する型
/// @details
/// @code
/// static result<State> load(const std::filesystem::path&)
/// static error         save(const State&, const std::filesystem::path&)
/// @endcode
template <class State, class Adapter>
concept adapter_for = requires(const std::filesystem::path& path, const State& state) {
  { Adapter::template load<State>(path) } -> std::same_as<glzdb::result<State>>;
  { Adapter::template save<State>(state, path) } -> std::same_as<glzdb::error>;
};

/// @brief 単一 JSON ファイルアダプタ (Unified)
///
/// 全 State を 1 つの JSON ファイルへ読み書きする。
struct JsonAdapter {
  /// @brief ファイルから State を読み込む。ファイル不存在時は空で生成
  /// @tparam State 状態型
  /// @param path ファイルパス
  /// @return 読み込んだ State、またはエラー
  template <class State>
  static result<State> load(const std::filesystem::path& path) {
    State state;
    if (auto res = detail::read_file_into(state, path); !res) {
      return std::unexpected(res.error());
    }
    return state;
  }

  /// @brief State を JSON ファイルへ atomic 書き込み
  /// @tparam State 状態型
  /// @param state 書き込み対象
  /// @param path ファイルパス
  /// @return 成功時は none、失敗時はエラーコード
  template <class State>
  static error save(const State& state, const std::filesystem::path& path) {
    return detail::write_atomic(state, path);
  }
};

/// @brief パーティショナル JSON アダプタ (1 テーブル = 1 ファイル)
///
/// 各テーブルを `<name_of<T>>.json` というファイル名で、
/// ディレクトリ `path` の下に出力・読み込みする。
struct JsonPartitionedAdapter {
  /// @brief ディレクトリ内の各テーブルファイルを読み込み State を復元
  /// @tparam State 状態型 (std::tuple<テーブル...>)
  /// @param path ディレクトリパス
  /// @return 読み込んだ State、またはエラー
  template <class State>
  static result<State> load(const std::filesystem::path& path) {
    State state;
    return load_impl(state, path, std::make_index_sequence<std::tuple_size_v<State>>{});
  }

  /// @brief ディレクトリ内にテーブル毎ファイルを atomic 書き込み
  /// @tparam State 状態型
  /// @param state 書き込み対象
  /// @param path ディレクトリパス
  /// @return 成功時は none、失敗時はエラーコード
  template <class State>
  static error save(const State& state, const std::filesystem::path& path) {
    std::error_code sys;
    std::filesystem::create_directories(path, sys);
    if (sys) {
      return error::io_error;
    }
    return save_impl(state, path, std::make_index_sequence<std::tuple_size_v<State>>{});
  }

  private:
  /// @brief テーブルファイルを順に読み込む (コンパイル時インデックス展開)
  /// @tparam State 状態型
  /// @tparam Is インデックスシーケンス
  /// @param state 読み込み先
  /// @param dir ディレクトリパス
  /// @param first_error 最初のエラーを保持するアウトパラメータ
  /// @return 成功時は State、失敗時は error を搬載した result
  template <class State, std::size_t... Is>
  static result<State> load_impl(State& state, const std::filesystem::path& dir, std::index_sequence<Is...>) {
    error first_error = {};
    ((load_one<State, Is>(state, dir, first_error)), ...);
    return first_error == error{} ? result<State>{std::move(state)} : result<State>{std::unexpected(first_error)};
  }

  /// @brief 1 つのテーブルファイルを読み込む
  /// @details 既にエラーが発生している場合は何もせず、最初のエラーを保持する。
  /// @tparam State 状態型
  /// @tparam I テーブルのタプルインデックス
  /// @param state 読み込み先
  /// @param dir ディレクトリパス
  /// @param first_error 最初のエラーを保持するアウトパラメータ
  template <class State, std::size_t I>
  static void load_one(State& state, const std::filesystem::path& dir, error& first_error) {
    if (first_error != error{}) {
      return;  // 既に失敗済み; 最初のエラーを保持
    }
    using T         = std::tuple_element_t<I, State>;
    auto&      rows = std::get<I>(state).rows;
    const auto file = dir / (std::string{name_of_v<typename T::value_type>} + ".json");
    auto       res  = detail::read_file_into(rows, file);
    if (!res) {
      first_error = res.error();  // ファイルが壊れている
    }
  }

  /// @brief テーブルファイルを順に書き込む (コンパイル時インデックス展開)
  /// @tparam State 状態型
  /// @tparam Is インデックスシーケンス
  template <class State, std::size_t... Is>
  static error save_impl(const State& state, const std::filesystem::path& dir, std::index_sequence<Is...>) {
    error first_error = {};
    ((save_one<State, Is>(state, dir, first_error)), ...);
    return first_error;
  }

  /// @brief 1 つのテーブルファイルへ atomic 書き込み
  /// @details 既にエラーが発生している場合は何もせず、最初のエラーを保持する。
  /// @tparam State 状態型
  /// @tparam I テーブルのタプルインデックス
  template <class State, std::size_t I>
  static void save_one(const State& state, const std::filesystem::path& dir, error& first_error) {
    if (first_error != error{}) {
      return;  // 既に失敗済み; 最初のエラーを保持
    }
    using T          = std::tuple_element_t<I, State>;
    const auto& rows = std::get<I>(state).rows;
    const auto  file = dir / (std::string{name_of_v<typename T::value_type>} + ".json");
    first_error      = detail::write_atomic(rows, file);
  }
};

}  // namespace glzdb
