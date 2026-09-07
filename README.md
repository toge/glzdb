# glzdb

[glaze](https://github.com/stephenberry/glaze) ベースの軽量 JSON データベースです。

SQLiteと同じようにシングルプロセス・インメモリで、JSON 永続化を持つ組み込み DB。ORM 的な操作性を目指します。
prototype・小規模アプリ・実験用であり、本番用途・大規模データ・並行アクセスは対象外です。

## 特徴

- マクロなし: C++ に derive マクロが無いため、`std::tuple` + 慣例 (`id` メンバ) + `std::get` で型安全ルーティング
- 依存: glaze のみ
- ヘッダーオンリーライブラリ
- C++23: 例外は内部で投げず `std::expected` で返す
- atomic永続化: 一時ファイルへ書いて `std::filesystem::rename` で置換 (同一 FS 上でatomic)
- 2 つの永続化形式: 単一 JSON ファイル (Unified) / テーブル毎ファイル (Partitioned)

## クイックスタート

```cpp
#include <glzdb.hpp>

struct User { uint64_t id; std::string name; };
struct Post { uint64_t id; uint64_t user_id; std::string title; };

using State = std::tuple<glzdb::table<User>, glzdb::table<Post>>;
using Db = glzdb::database<State, glzdb::JsonAdapter>;

int main() {
  auto db = Db::open("db.json");          // 既存ロード or 空で生成
  if (!db) {
    return 1;
  }

  db->insert(User{1, "alice"});
  db->insert(Post{10, 1, "first post"});
  db->flush();                             // 明示書き出し (破棄時も自動)

  const auto* u = db->get<User>(1);
  for (const auto* p : db->related<&Post::user_id>(1)) {  // has_many
    // ...
  }
}
```

## モデル定義 (マクロなし)

```cpp
struct User { uint64_t id; std::string name; };
struct Post { uint64_t id; uint64_t user_id; std::string title; };
```

- `id_type<T> = decltype(std::declval<T>().id)` — SFINAE/concept (`has_id_member<T>`) で検出
- 基底クラス・マクロ・CRTP 不要。glaze が構造体を直接 (de)serialize
- 上書きフック `glzdb::id_traits<T>` は v1 では未提供 (id メンバは `id` 固定)

## テーブルとステート

```cpp
template<class T> struct table {
  using id_type = glzdb::id_type<T>;
  std::map<id_type, T> rows;
};

using State = std::tuple<table<User>, table<Post>>;
```

- テーブルは `std::map` (ordered)。`std::unordered_map` への切替はしない
- glaze は `std::tuple` の構造体を直接 (de)serialize できる

## データベース本体

```cpp
template<class State, class Adapter> struct database {
  State state;
  std::filesystem::path path;

  static auto open(const std::filesystem::path&);   // 既存ロード or 空で生成
  ~database();                                       // flush() (flush_on_destruct, 既定 true)
  error flush() const;                               // 全状態を書き出し
  template<class T> table<T>& tables();              // 型安全ルーティング
};
```

- ルーティングは `std::get<table<T>>` — コンパイル時・登録不要・型安全

## CRUD API

| 操作       | シグネチャ                                                                      |
| ---------- | ------------------------------------------------------------------------------- |
| insert     | `insert<T>(const T&) -> std::expected<void, error>` (id 重複で `duplicate_key`) |
| upsert     | `upsert<T>(const T&) -> std::expected<void, error>`                             |
| get        | `get<T>(id) -> const T*` (不在なら nullptr)                                     |
| get_all    | `get_all<T>() -> テーブル値の range` (コピーなし)                               |
| get_all_by | `get_all_by<&User::name>("Alice") -> std::vector<const T*>` (メンバポインタ)    |
| update     | `update<T>(const T&) -> std::expected<void, error>` (id 不在で `not_found`)     |
| remove     | `remove<T>(id) -> bool`                                                         |
| count      | `count<T>() -> std::size_t`                                                     |
| auto-id    | v2 (`next_id<T>()` max+1 / `insert_new<T>`)。v1 は明示 id                       |

## アダプタ

| アダプタ                 | 形式 | 型                                                         |
| ------------------------ | ---- | ---------------------------------------------------------- |
| `JsonAdapter`            | JSON | Unified (単一ファイル = 全状態)                            |
| `JsonPartitionedAdapter` | JSON | Partitioned (ディレクトリ, テーブル毎 `<name_of<T>>.json`) |

- `name_of<T>` の既定値は `glz::name_v<T>`、`state_meta` で上書き可能
- Partitioned の書き込み: タプルをコンパイル時走査して各テーブルを個別ファイルに出力
- Partitioned の読み込み: タプル走査で既知テーブルのファイルを読む (動的 name→type マップ不要)

## リレーション (簡易 range ヘルパのみ)

```cpp
db.related<&Post::user_id>(user.id);      // has_many: user の子
db.parent<User, &Post::user_id>(post);    // belongs_to: post の親
```

- `get_all_by` 上の糖衣。スキーマ・遅延読み込み・カスケード・整合性保証なし

## 永続化と安全性

- 全書き込み: 一時ファイルへ書いて `std::filesystem::rename` で置換 (同一 FS 上で原子)
- Partitioned はファイル単位で原子、DB全体では非原子 (途中クラッシュで新旧混在し得る)。全体原子性が必要なら Unified を使うこと
- `open` で一度だけ読み込み。増分同期なし
- シングルプロセス前提、ファイルロックなし (`// ponytail: single-process` コメント)
- `flush_on_destruct` トグル (既定 true)

## エラーハンドリング

- `std::expected<T, glzdb::error>` でフォールバブル操作を返す
- `error` = `enum class { none, io_error, duplicate_key, not_found, parse_error }` + `message()`
- glaze のパースエラーは `parse_error` にマップ

## ビルド

vcpkg で glaze と catch2 を導入済みの環境が必要です。

## テスト (Catch2)

- open/flush ラウンドトリップ
- CRUD 一式 (insert/get/update/remove/count)
- `get_all_by` フィルタ
- Partitioned ラウンドトリップ
- リレーションヘルパ
- 重複キー・欠落 id エラー
- 破損ファイル耐性 (temp+rename)
- `demo()` セルフチェックも併設

## 対象外の機能

- CSV/RON アダプタ
- トランザクション
- インデックス
- マルチプロセスロック
- 遅延/カスケードリレーション
- スキーママイグレーション
- 非同期 I/O、auto-id

## ライセンス

MIT
