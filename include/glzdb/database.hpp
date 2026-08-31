#pragma once

#include <filesystem>
#include <ranges>
#include <type_traits>
#include <utility>
#include <vector>

#include "adapters.hpp"
#include "error.hpp"
#include "table.hpp"
#include "traits.hpp"

namespace glzdb {

/// @brief データベース本体
///
/// State タプル (テーブルの集合) を保持し、アダプタを通して永続化する。
/// 全ての変更はメモリ上で行われ、flush() でディスクへ書き出す。
///
/// @tparam State テーブルの std::tuple (各要素は table<T>)
/// @tparam Adapter 永続化アダプタ (adapter_for concept を満たす型)
///
/// @code
/// database<State, JsonAdapter>::open("db.json")
/// database<State, JsonPartitionedAdapter>::open("dbdir")
/// @endcode
template <class State, class Adapter>
  requires std::is_object_v<State> && adapter_for<State, Adapter>
struct database {
  State                 state;           ///< 保持中のテーブル群
  std::filesystem::path path;            ///< 永続化先パス

  // コピーは二重 flush を招くため禁止、ムーブのみ許可
  database(const database&)            = delete;
  database& operator=(const database&) = delete;
  database(database&&) noexcept        = default;
  database& operator=(database&&) noexcept = default;

  /// @brief データベースを開く (または新規作成)
  ///
  /// 既存ファイルがあれば一度だけ読み込む。読み込み失敗時はエラーを返す。
  /// @param path 永続化ファイル (またはディレクトリ) のパス
  /// @return 開いたデータベース、またはエラー
  static result<database> open(const std::filesystem::path& path) {
    auto loaded = Adapter::template load<State>(path);
    if (!loaded) {
      return std::unexpected(loaded.error());
    }
    return database(std::move(*loaded), path, true);
  }

  /// @brief デストラクタ — flush_on_destruct が真なら破壊時に状態をフラッシュ
  ~database() {
    if (flush_on_destruct_) {
      (void)flush();
    }
  }

  /// @brief 全状態をアダプタ経由で永続化
  /// @return 成功時は none、失敗時はエラーコード
  error flush() const { return Adapter::template save<State>(state, path); }

  /// @brief 破棄時自動フラッシュの切り替え
  /// @param v true でデストラクタ時に自動 flush
  void set_flush_on_destruct(bool v) noexcept { flush_on_destruct_ = v; }
  /// @brief 破棄時自動フラッシュが有効か
  [[nodiscard]] bool flush_on_destruct() const noexcept { return flush_on_destruct_; }

 private:
  bool flush_on_destruct_ = true;  ///< デストラクタでの自動 flush 有効フラグ

  /// @brief 内部用コンストラクタ
  database(State&& s, std::filesystem::path p, bool do_flush)
      : state(std::move(s)), path(std::move(p)), flush_on_destruct_(do_flush) {}

 public:

  /// @brief 型安全ルーティング: コンパイル時にテーブルを取得 (登録不要)
  /// @tparam T モデル型
  /// @return T に対応するテーブルへの参照
  template <class T>
  table<T>& tables() {
    return std::get<table<T>>(state);
  }
  template <class T>
  const table<T>& tables() const {
    return std::get<table<T>>(state);
  }

  // -- CRUD (作成 / 読取 / 更新 / 削除) ---------------------------------

  /// @brief 行を挿入
  /// @note id が既存の場合は duplicate_key エラー
  /// @tparam T モデル型
  /// @param row 挿入する行
  /// @return 成功時は空の expected、失敗時はエラー
  template <class T>
  result<void> insert(const T& row) {
    auto& rows                = tables<T>().rows;
    const auto [it, inserted] = rows.emplace(row.id, row);
    if (!inserted) {
      return std::unexpected(error::duplicate_key);
    }
    return {};
  }

  /// @brief 行を挿入または置換 (id 衝突なし)
  /// @tparam T モデル型
  /// @param row 挿入/更新する行
  /// @return 成功時は空の expected
  template <class T>
  result<void> upsert(const T& row) {
    tables<T>().rows[row.id] = row;
    return {};
  }

  /// @brief id により行を検索
  /// @tparam T モデル型
  /// @param id 主キー
  /// @return 見つかった行へのポインタ、不存在時は nullptr
  /// @warning 返却ポインタは `std::map` ノードを指す。次の `insert`/`update` は安定だが
  ///          `remove` で該当行を削除するとダングリングになる。次の書き込みまで有効と考えること
  template <class T>
  const T* get(const id_type<T>& id) const {
    const auto& rows = tables<T>().rows;
    if (const auto it = rows.find(id); it != rows.end()) {
      return &it->second;
    }
    return nullptr;
  }

  /// @brief 全行を値の range として取得 (コピーなし)
  /// @tparam T モデル型
  /// @return マップ値のビュー
  /// @warning ビューは `state` 内の `std::map` を直接参照する。`insert`/`remove` でイテレータが
  ///          無効化される可能性があるため、書き込みと同時走査は避けること
  template <class T>
  auto get_all() const {
    const auto& rows = tables<T>().rows;
    return rows | std::views::values;
  }

  /// @brief 指定メンバが `value` に一致する行をポインタの vector で取得
  /// @note 順序はマップ (id) の順序に従う
  /// @tparam Member メンバポインタ (例: &User::name)
  /// @param value 一致させる値
  /// @return マッチする行へのポインタのベクタ
  /// @warning 返却ポインタは `std::map` ノードを指す。`remove` で該当行を削除するとダングリングになる
  template <auto Member>
  std::vector<const member_owner_t<Member>*> get_all_by(const member_value_t<Member>& value) const
    requires std::is_member_object_pointer_v<decltype(Member)>
  {
    using T = member_owner_t<Member>;
    std::vector<const T*> out;
    for (const auto& row : tables<T>().rows) {
      if (row.second.*Member == value) {
        out.push_back(&row.second);
      }
    }
    return out;
  }

  /// @brief 行を置換
  /// @note id が存在しない場合は not_found エラー
  /// @tparam T モデル型
  /// @param row 更新する行
  /// @return 成功時は空の expected、失敗時はエラー
  template <class T>
  result<void> update(const T& row) {
    auto& rows = tables<T>().rows;
    if (const auto it = rows.find(row.id); it != rows.end()) {
      it->second = row;
      return {};
    }
    return std::unexpected(error::not_found);
  }

  /// @brief id により行を削除
  /// @tparam T モデル型
  /// @param id 主キー
  /// @return 削除された場合は true、存在しない場合は false
  template <class T>
  bool remove(const id_type<T>& id) {
    return tables<T>().rows.erase(id) > 0;
  }

  /// @brief 行数を取得
  /// @tparam T モデル型
  /// @return テーブル内の行数
  template <class T>
  std::size_t count() const {
    return tables<T>().rows.size();
  }

  // -- リレーション (get_all_by の薄いラッパ) ----------------------

  /// @brief has_many: `Member` で親 `parent_id` に紐づく子行を取得
  /// @details 例: db.related<&Post::user_id>(user.id)
  /// @tparam Member 外部キーとなるメンバポインタ
  /// @param parent_id 親の主キー
  /// @return 子行へのポインタのベクタ
  /// @warning 返却ポインタは `std::map` ノードを指す。`remove` で子行を削除するとダングリングになる
  template <auto Member>
  std::vector<const member_owner_t<Member>*> related(const member_value_t<Member>& parent_id) const
    requires std::is_member_object_pointer_v<decltype(Member)>
  {
    return get_all_by<Member>(parent_id);
  }

  /// @brief belongs_to: `child` の親を 1 件取得 (見つからなければ nullptr)
  /// @details 親テーブルは Parent、子の外部キーは Member。
  ///          例: db.parent<User, &Post::user_id>(post)
  /// @tparam Parent 親モデル型
  /// @tparam Member 外部キーとなるメンバポインタ
  /// @tparam Child 子モデル型
  /// @param child 子行
  /// @return 親行へのポインタ、見つからなければ nullptr
  /// @warning 返却ポインタは `std::map` ノードを指す。親を `remove` するとダングリングになる
  template <class Parent, auto Member, class Child>
  const Parent* parent(const Child& child) const
    requires std::is_member_object_pointer_v<decltype(Member)>
  {
    static_assert(std::same_as<member_owner_t<Member>, Child>,
                  "Member は Child のメンバでなければならない");
    static_assert(std::same_as<member_value_t<Member>, id_type<Parent>>,
                  "Member の値型は Parent の id_type と一致しなければならない");
    const auto& rows = tables<Parent>().rows;
    if (const auto it = rows.find(child.*Member); it != rows.end()) {
      return &it->second;
    }
    return nullptr;
  }
};

}  // namespace glzdb
