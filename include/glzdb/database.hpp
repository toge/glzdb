#pragma once

#include <filesystem>
#include <functional>
#include <ranges>
#include <type_traits>
#include <utility>
#include <vector>

#include <glaze/json/read.hpp>
#include <glaze/json/write.hpp>

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
  // ムーブ元は flush_on_destruct_ を false に倒す (空 path への無駄 flush 防止)
  database(const database&)            = delete;
  database& operator=(const database&) = delete;
  database(database&& o) noexcept
      : state(std::move(o.state)),
        path(std::move(o.path)),
        flush_on_destruct_(std::exchange(o.flush_on_destruct_, false)) {}
  database& operator=(database&& o) noexcept {
    if (this != &o) {
      state              = std::move(o.state);
      path               = std::move(o.path);
      flush_on_destruct_ = std::exchange(o.flush_on_destruct_, false);
    }
    return *this;
  }

  /// @brief マイグレーション関数型
  /// @details State を受け取り、in-place で変更する関数。
  ///          open() 時に読み込み後に呼ばれる。
  template <class S = State>
  using migrate_fn = std::function<void(S&)>;

  /// @brief データベースを開く (または新規作成)
  ///
  /// 既存ファイルがあれば一度だけ読み込む。読み込み失敗時はエラーを返す。
  /// migrate が指定された場合、読み込み後に呼ばれる。
  /// @param path 永続化ファイル (またはディレクトリ) のパス
  /// @param migrate マイグレーション関数 (nullptr なら呼ばれない)
  /// @return 開いたデータベース、またはエラー
  [[nodiscard]] static result<database> open(const std::filesystem::path& path,
                                             migrate_fn<> migrate = nullptr) {
    auto loaded = Adapter::template load<State>(path);
    if (!loaded) {
      return std::unexpected(loaded.error());
    }
    if (migrate) {
      migrate(*loaded);
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
  [[nodiscard]] error flush() const { return Adapter::template save<State>(state, path); }

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
  [[nodiscard]] result<void> insert(const T& row) {
    auto& rows                = tables<T>().rows;
    const auto [it, inserted] = rows.emplace(row.id, row);
    if (!inserted) {
      return std::unexpected(error::duplicate_key);
    }
    invoke_on_insert(row);
    return {};
  }

  /// @brief 行を挿入または置換 (id 衝突なし)
  /// @tparam T モデル型
  /// @param row 挿入/更新する行
  /// @return 成功時は空の expected
  template <class T>
  [[nodiscard]] result<void> upsert(const T& row) {
    tables<T>().rows[row.id] = row;
    invoke_on_insert(row);
    return {};
  }

  /// @brief id により行を検索
  /// @tparam T モデル型
  /// @param id 主キー
  /// @return 見つかった行へのポインタ、不存在時は nullptr
  /// @warning 返却ポインタは `std::map` ノードを指す。次の `insert`/`update` は安定だが
  ///          `remove` で該当行を削除するとダングリングになる。次の書き込みまで有効と考えること
  template <class T>
  [[nodiscard]] const T* get(const id_type<T>& id) const {
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
  [[nodiscard]] auto get_all() const {
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
  [[nodiscard]] std::vector<const member_owner_t<Member>*> get_all_by(const member_value_t<Member>& value) const
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
  [[nodiscard]] result<void> update(const T& row) {
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
  [[nodiscard]] bool remove(const id_type<T>& id) {
    auto& rows = tables<T>().rows;
    if (const auto it = rows.find(id); it != rows.end()) {
      invoke_on_remove(it->second);
      rows.erase(it);
      return true;
    }
    return false;
  }

  /// @brief 行数を取得
  /// @tparam T モデル型
  /// @return テーブル内の行数
  template <class T>
  [[nodiscard]] std::size_t count() const {
    return tables<T>().rows.size();
  }

  // -- auto-id --------------------------------------------------------

  /// @brief 次に割り当てる id を返す (max(id)+1、空なら 1)
  /// @tparam T モデル型
  /// @return 次の id
  template <class T>
  [[nodiscard]] id_type<T> next_id() const {
    const auto& map = tables<T>().rows;
    if (map.empty()) return id_type<T>{1};
    return map.rbegin()->first + id_type<T>{1};
  }

  /// @brief id を自動付与して挿入、割り当てた id を返す
  /// @tparam T モデル型
  /// @param row 挿入する行 (id は上書きされる)
  /// @return 割り当てた id、またはエラー
  template <class T>
  [[nodiscard]] result<id_type<T>> insert_new(T row) {
    const id_type<T> id = next_id<T>();
    row.id = id;
    auto err = insert(row);
    if (!err) return std::unexpected(err.error());
    return id;
  }

  // -- バッチ操作 -----------------------------------------------------

  /// @brief 範囲内の行を一括挿入
  /// @note 重複キーが見つかった時点で中断し、それまでに挿入済みの行は残る
  /// @tparam T モデル型
  /// @tparam R 行の範囲型
  /// @param rows 挿入する行の範囲
  /// @return 成功時は空の expected、失敗時はエラー
  template <class T, std::ranges::range R>
  [[nodiscard]] result<void> insert_range(R&& rows) {
    auto& map = tables<T>().rows;
    for (auto&& row : rows) {
      const auto& id = row.id;
      if (map.contains(id)) {
        return std::unexpected(error::duplicate_key);
      }
      map.emplace(id, std::forward<decltype(row)>(row));
    }
    return {};
  }

  /// @brief 述語に一致する行を一括削除
  /// @tparam T モデル型
  /// @tparam Pred 述語型
  /// @param pred 削除する行に適用する述語
  /// @return 削除した行数
  template <class T, std::predicate<const T&> Pred>
  [[nodiscard]] std::size_t remove_if(Pred&& pred) {
    auto& map = tables<T>().rows;
    std::size_t count = 0;
    for (auto it = map.begin(); it != map.end();) {
      if (pred(it->second)) {
        it = map.erase(it);
        ++count;
      } else {
        ++it;
      }
    }
    return count;
  }

  // -- 述語ベース検索 -------------------------------------------------

  /// @brief 述語に一致する最初の行を返す
  /// @tparam T モデル型
  /// @tparam Pred 述語型
  /// @param pred 検索条件
  /// @return 見つかった行へのポインタ、不存在時は nullptr
  /// @warning 返却ポインタは `std::map` ノードを指す。`remove` でダングリングになる
  template <class T, std::predicate<const T&> Pred>
  [[nodiscard]] const T* find_if(Pred&& pred) const {
    const auto& map = tables<T>().rows;
    for (const auto& [id, row] : map) {
      if (pred(row)) return &row;
    }
    return nullptr;
  }

  /// @brief 述語に一致する全行を返す
  /// @tparam T モデル型
  /// @tparam Pred 述語型
  /// @param pred 検索条件
  /// @return マッチする行へのポインタのベクタ
  /// @warning 返却ポインタは `std::map` ノードを指す。`remove` でダングリングになる
  template <class T, std::predicate<const T&> Pred>
  [[nodiscard]] std::vector<const T*> find_all_if(Pred&& pred) const {
    const auto& map = tables<T>().rows;
    std::vector<const T*> result;
    for (const auto& [id, row] : map) {
      if (pred(row)) result.push_back(&row);
    }
    return result;
  }

  // -- リレーション (get_all_by の薄いラッパ) ----------------------

  /// @brief has_many: `Member` で親 `parent_id` に紐づく子行を取得
  /// @details 例: db.related<&Post::user_id>(user.id)
  /// @tparam Member 外部キーとなるメンバポインタ
  /// @param parent_id 親の主キー
  /// @return 子行へのポインタのベクタ
  /// @warning 返却ポインタは `std::map` ノードを指す。`remove` で子行を削除するとダングリングになる
  template <auto Member>
  [[nodiscard]] std::vector<const member_owner_t<Member>*> related(const member_value_t<Member>& parent_id) const
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
  [[nodiscard]] const Parent* parent(const Child& child) const
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

  // -- 外部キー整合性 -------------------------------------------------

  /// @brief 外部キー違反時の削除挙動
  enum class on_delete { cascade, restrict, nullify };

  /// @brief 参照整合性付きで親行を削除
  /// @details 子行が存在する場合、policy に従って処理:
  ///          - restrict: 削除を拒否 (foreign_key_violation)
  ///          - cascade: 子行も連鎖削除
  ///          - nullify: 子の外部キーを 0 に設定
  /// @tparam Parent 親モデル型
  /// @tparam FK_Member 子の外部キーメンバポインタ
  /// @tparam Child 子モデル型
  /// @param id 削除する親の id
  /// @param policy 削除挙動 (既定は restrict)
  /// @return 成功時は空の expected、失敗時はエラー
  template <class Parent, auto FK_Member, class Child>
  [[nodiscard]] result<void> remove_parent(const id_type<Parent>& id,
                                           on_delete policy = on_delete::restrict) {
    static_assert(std::is_same_v<member_value_t<FK_Member>, id_type<Parent>>,
                  "FK_Member の値型は Parent の id_type と一致しなければならない");
    static_assert(std::is_same_v<member_owner_t<FK_Member>, Child>,
                  "FK_Member は Child のメンバでなければならない");

    auto children = related<FK_Member>(id);

    if (!children.empty()) {
      switch (policy) {
      case on_delete::restrict:
        return std::unexpected(error::foreign_key_violation);
      case on_delete::cascade:
        for (const auto* child : children) {
          remove<Child>(child->id);
        }
        break;
      case on_delete::nullify:
        for (const auto* child : children) {
          Child copy = *child;
          copy.*FK_Member = {};
          update(copy);
        }
        break;
      }
    }

    remove<Parent>(id);
    return {};
  }

  // -- シリアライズ補助 -------------------------------------------------

  /// @brief テーブルを行単位 JSON 文字列にシリアライズ
  /// @tparam T モデル型
  /// @return JSON 文字列、またはエラー
  template <class T>
  [[nodiscard]] result<std::string> to_json() const {
    const auto& map = tables<T>().rows;
    std::string buffer;
    auto ec = glz::write_json(map, buffer);
    if (bool(ec)) return std::unexpected(error::parse_error);
    return buffer;
  }

  /// @brief JSON 文字列からテーブルを復元 (既存データは置換)
  /// @tparam T モデル型
  /// @param json JSON 文字列
  /// @return 成功時は空の expected、失敗時はエラー
  template <class T>
  [[nodiscard]] result<void> from_json(const std::string& json) {
    auto& map = tables<T>().rows;
    std::map<id_type<T>, T> new_rows;
    auto ec = glz::read_json(new_rows, json);
    if (bool(ec)) return std::unexpected(error::parse_error);
    map = std::move(new_rows);
    return {};
  }

  // -- メモリ最適化 ---------------------------------------------------

  /// @brief 全テーブルの std::map を再構築してメモリ最適化
  /// @details 大量 delete 後にメモリ使用量を削減する
  void compact() {
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
      (compact_at<Is>(), ...);
    }(std::make_index_sequence<std::tuple_size_v<State>>{});
  }

  // -- オブザーバーコールバック (hooks) --------------------------------

  /// @brief フック関数型
  template <class T>
  using hook_fn = std::function<void(const T&)>;

  /// @brief insert 時に呼ばれるフックを登録
  /// @tparam T モデル型
  /// @param cb コールバック関数
  template <class T>
  void set_on_insert(hook_fn<T> cb) {
    std::get<hook_pair<T>>(hooks_).on_insert = std::move(cb);
  }

  /// @brief remove 時に呼ばれるフックを登録
  /// @tparam T モデル型
  /// @param cb コールバック関数
  template <class T>
  void set_on_remove(hook_fn<T> cb) {
    std::get<hook_pair<T>>(hooks_).on_remove = std::move(cb);
  }

  private:
  // hooks_ のための型抽出
  template <class S>
  struct extract_types;
  template <class... Ts>
  struct extract_types<std::tuple<table<Ts>...>> {
    using type = std::tuple<Ts...>;
  };

  template <class T>
  struct hook_pair {
    hook_fn<T> on_insert{};
    hook_fn<T> on_remove{};
  };
  template <class Tuple>
  struct apply_hooks;
  template <class... Ts>
  struct apply_hooks<std::tuple<Ts...>> {
    using type = std::tuple<hook_pair<Ts>...>;
  };

  using hooks_t = typename apply_hooks<typename extract_types<State>::type>::type;
  hooks_t hooks_;

  public:
  // -- 内部用ヘルパ -----------------------------------------------

  private:
  /// @brief 指定インデックスのテーブルを compact
  template <std::size_t I>
  void compact_at() {
    using table_type = std::tuple_element_t<I, State>;
    using T          = typename table_type::value_type;
    auto& map        = std::get<I>(state).rows;
    std::map<id_type<T>, T> rebuilt{map.begin(), map.end()};
    map.swap(rebuilt);
  }

  /// @brief insert フックを呼び出す
  template <class T>
  void invoke_on_insert(const T& row) {
    auto& hook = std::get<hook_pair<T>>(hooks_);
    if (hook.on_insert) hook.on_insert(row);
  }

  /// @brief remove フックを呼び出す
  template <class T>
  void invoke_on_remove(const T& row) {
    auto& hook = std::get<hook_pair<T>>(hooks_);
    if (hook.on_remove) hook.on_remove(row);
  }
};

}  // namespace glzdb
