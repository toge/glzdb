#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <tuple>

#include <catch2/catch_all.hpp>

#include <glzdb.hpp>

namespace {

/// @brief 主キー (`id`) を持つモデル
struct User {
  uint64_t    id;
  std::string name;
};

struct Post {
  uint64_t    id;
  uint64_t    user_id;
  std::string title;
};

using State = std::tuple<glzdb::table<User>, glzdb::table<Post>>;
using Db    = glzdb::database<State, glzdb::JsonAdapter>;

User make_user(const uint64_t id, const std::string& name) {
  return {id, name};
}

/// @brief 並列実行でも衝突しない一意な一時ディレクトリを生成
inline std::filesystem::path make_temp_dir(const std::string_view prefix) {
  static std::atomic<uint64_t> counter{0};
  const auto ts  = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  const auto cnt = counter.fetch_add(1);
  return std::filesystem::temp_directory_path() /
         (std::string(prefix) + "_" + std::to_string(ts) + "_" + std::to_string(cnt));
}
}  // namespace

TEST_CASE("traits: id_type と name_of") {
  STATIC_REQUIRE(std::same_as<glzdb::id_type<User>, uint64_t>);
  STATIC_REQUIRE(std::same_as<glzdb::id_type<Post>, uint64_t>);
  STATIC_REQUIRE(glzdb::has_id_member<User>);
  STATIC_REQUIRE(glzdb::name_of_v<User> == "User");
  STATIC_REQUIRE(glzdb::name_of_v<Post> == "Post");
}

TEST_CASE("database: open で空の db が生成され CRUD 一周") {
  const auto dir = make_temp_dir("glzdb_test_open");
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto path = dir / "db.json";

  {
    auto db = Db::open(path);
    REQUIRE(db);
    REQUIRE(db->count<User>() == 0);

    auto ins = db->insert(make_user(1, "alice"));
    REQUIRE(ins);
    REQUIRE(db->count<User>() == 1);

    const auto* u = db->get<User>(1);
    REQUIRE(u != nullptr);
    REQUIRE(u->name == "alice");

    REQUIRE(db->get<User>(2) == nullptr);
  }
  // db 破棄 -> flush() がファイルへ書き出し

  {
    auto db = Db::open(path);
    REQUIRE(db);
    REQUIRE(db->count<User>() == 1);
    REQUIRE(db->get<User>(1)->name == "alice");
  }

  std::filesystem::remove_all(dir);
}

TEST_CASE("database: flush() が明示的に永続化する") {
  const auto dir = make_temp_dir("glzdb_test_flush");
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto path = dir / "db.json";

  {
    auto db = Db::open(path);
    REQUIRE(db);
    REQUIRE(db->insert(make_user(7, "bob")));
    REQUIRE(db->flush() == glzdb::error::none);
  }

  {
    auto db = Db::open(path);
    REQUIRE(db);
    REQUIRE(db->get<User>(7) != nullptr);
  }

  std::filesystem::remove_all(dir);
}

TEST_CASE("database: CRUD insert/get/update/remove/count") {
  const auto dir = make_temp_dir("glzdb_test_crud");
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);

  auto db = Db::open(dir / "db.json");
  REQUIRE(db);

  // insert (挿入)
  REQUIRE(db->insert(make_user(1, "alice")));
  REQUIRE(db->insert(make_user(2, "bob")));
  REQUIRE(db->count<User>() == 2);

  // 重複キー
  auto dup = db->insert(make_user(1, "alice2"));
  REQUIRE(!dup);
  REQUIRE(dup.error() == glzdb::error::duplicate_key);
  REQUIRE(db->count<User>() == 2);

  // get (検索)
  REQUIRE(db->get<User>(1)->name == "alice");
  REQUIRE(db->get<User>(99) == nullptr);

  // update (更新)
  REQUIRE(db->update(make_user(2, "robert")));
  REQUIRE(db->get<User>(2)->name == "robert");

  // 存在しない id の update
  auto missing = db->update(make_user(99, "nobody"));
  REQUIRE(!missing);
  REQUIRE(missing.error() == glzdb::error::not_found);

  // upsert: 挿入後に置換
  REQUIRE(db->upsert(make_user(3, "carol")));
  REQUIRE(db->upsert(make_user(3, "carol2")));
  REQUIRE(db->count<User>() == 3);
  REQUIRE(db->get<User>(3)->name == "carol2");

  // remove (削除)
  REQUIRE(db->remove<User>(1));
  REQUIRE(!db->remove<User>(1));
  REQUIRE(db->count<User>() == 2);

  std::filesystem::remove_all(dir);
}

TEST_CASE("database: get_all と get_all_by") {
  const auto dir = make_temp_dir("glzdb_test_getall");
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);

  auto db = Db::open(dir / "db.json");
  REQUIRE(db);
  REQUIRE(db->insert(make_user(1, "alice")));
  REQUIRE(db->insert(make_user(2, "bob")));
  REQUIRE(db->insert(make_user(3, "alice")));

  // get_all: マップの値を range で走査 (コピーなし)
  {
    auto                     all = db->get_all<User>();
    std::vector<std::string> names;
    for (const auto& u : all) {
      names.push_back(u.name);
    }
    REQUIRE(names == std::vector<std::string>{"alice", "bob", "alice"});
  }

  // get_all_by: メンバポインタフィルタ
  {
    auto by_name = db->get_all_by<&User::name>("alice");
    REQUIRE(by_name.size() == 2);
    REQUIRE(by_name[0]->id == 1);
    REQUIRE(by_name[1]->id == 3);
  }

  std::filesystem::remove_all(dir);
}

TEST_CASE("database: リレーション (related / parent)") {
  const auto dir = make_temp_dir("glzdb_test_rel");
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);

  auto db = Db::open(dir / "db.json");
  REQUIRE(db);
  REQUIRE(db->insert(make_user(1, "alice")));
  REQUIRE(db->insert(User{2, "bob"}));
  REQUIRE(db->insert(Post{10, 1, "hello"}));
  REQUIRE(db->insert(Post{11, 1, "world"}));
  REQUIRE(db->insert(Post{12, 2, "other"}));

  // has_many: ユーザー 1 の投稿
  {
    auto posts = db->related<&Post::user_id>(1);
    REQUIRE(posts.size() == 2);
    REQUIRE(posts[0]->id == 10);
    REQUIRE(posts[1]->id == 11);
  }

  // belongs_to: 投稿 10 の親
  {
    const auto* u = db->parent<User, &Post::user_id>(*db->get<Post>(10));
    REQUIRE(u != nullptr);
    REQUIRE(u->id == 1);
  }

  std::filesystem::remove_all(dir);
}

TEST_CASE("database: 破損ファイル -> open 時に parse_error") {
  const auto dir = make_temp_dir("glzdb_test_corrupt");
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto path = dir / "db.json";

  {
    std::ofstream f(path);
    f << "{ not valid json";
  }

  auto db = Db::open(path);
  REQUIRE(!db);
  REQUIRE(db.error() == glzdb::error::parse_error);

  std::filesystem::remove_all(dir);
}

TEST_CASE("partitioned アダプタ: テーブル毎ファイルのラウンドトリップ") {
  const auto dir = make_temp_dir("glzdb_test_part");
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto dbdir = dir / "dbdir";

  using Pdb = glzdb::database<State, glzdb::JsonPartitionedAdapter>;

  {
    auto db = Pdb::open(dbdir);
    REQUIRE(db);
    REQUIRE(db->insert(make_user(1, "alice")));
    REQUIRE(db->insert(Post{10, 1, "hello"}));
    REQUIRE(db->flush() == glzdb::error::none);

    // テーブル毎のファイルが存在する
    REQUIRE(std::filesystem::exists(dbdir / "User.json"));
    REQUIRE(std::filesystem::exists(dbdir / "Post.json"));
  }

  {
    auto db = Pdb::open(dbdir);
    REQUIRE(db);
    REQUIRE(db->count<User>() == 1);
    REQUIRE(db->count<Post>() == 1);
    REQUIRE(db->get<User>(1)->name == "alice");
    REQUIRE(db->get<Post>(10)->title == "hello");
  }

  std::filesystem::remove_all(dir);
}

TEST_CASE("partitioned アダプタ: テーブルファイルが無い場合は空で開始") {
  const auto dir = make_temp_dir("glzdb_test_part_missing");
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto dbdir = dir / "dbdir";

  using Pdb = glzdb::database<State, glzdb::JsonPartitionedAdapter>;

  {
    auto db = Pdb::open(dbdir);
    REQUIRE(db);
    REQUIRE(db->count<User>() == 0);
    REQUIRE(db->count<Post>() == 0);
  }

  std::filesystem::remove_all(dir);
}

TEST_CASE("error: message 文字列は空でない") {
  REQUIRE(!glzdb::message(glzdb::error::io_error).empty());
  REQUIRE(!glzdb::message(glzdb::error::duplicate_key).empty());
  REQUIRE(!glzdb::message(glzdb::error::not_found).empty());
  REQUIRE(!glzdb::message(glzdb::error::parse_error).empty());
}

TEST_CASE("database: コピー不可・ムーブ可能") {
  STATIC_REQUIRE(!std::is_copy_constructible_v<Db>);
  STATIC_REQUIRE(!std::is_copy_assignable_v<Db>);
  STATIC_REQUIRE(std::is_move_constructible_v<Db>);
  STATIC_REQUIRE(std::is_move_assignable_v<Db>);
}

TEST_CASE("database: ムーブ元のデストラクタは flush しない") {
  const auto dir = make_temp_dir("glzdb_test_move");
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto path = dir / "db.json";

  {
    auto db = Db::open(path);
    REQUIRE(db);
    REQUIRE(db->insert(make_user(1, "alice")));

    Db moved(std::move(*db));
    REQUIRE(!db->flush_on_destruct());  // ムーブ元は武装解除
    REQUIRE(moved.count<User>() == 1);
    REQUIRE(moved.flush() == glzdb::error::none);

    // ムーブ代入でもムーブ元は武装解除される
    auto other = Db::open(dir / "other.json");
    REQUIRE(other);
    *other = std::move(moved);
    REQUIRE(!moved.flush_on_destruct());
    REQUIRE(other->count<User>() == 1);
  }

  std::filesystem::remove_all(dir);
}

TEST_CASE("database: set_flush_on_destruct で自動 flush を無効化") {
  const auto dir  = make_temp_dir("glzdb_test_noflush");
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto path = dir / "db.json";

  {
    auto db = Db::open(path);
    REQUIRE(db);
    db->set_flush_on_destruct(false);
    REQUIRE(!db->flush_on_destruct());
    REQUIRE(db->insert(make_user(1, "alice")));
    // デストラクタで flush されない
  }
  {
    auto db = Db::open(path);
    REQUIRE(db);
    // flush されていないので空
    REQUIRE(db->count<User>() == 0);
  }

  std::filesystem::remove_all(dir);
}

TEST_CASE("partitioned: 破損ファイル -> parse_error") {
  const auto dir = make_temp_dir("glzdb_test_part_corrupt");
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto dbdir = dir / "dbdir";
  std::filesystem::create_directories(dbdir);

  // User.json に不正 JSON を書き込む
  {
    std::ofstream f(dbdir / "User.json");
    f << "{ not valid json }";
  }

  using Pdb = glzdb::database<State, glzdb::JsonPartitionedAdapter>;
  auto db   = Pdb::open(dbdir);
  REQUIRE(!db);
  REQUIRE(db.error() == glzdb::error::parse_error);

  std::filesystem::remove_all(dir);
}

TEST_CASE("partitioned: 部分的に破損したテーブルでも先頭エラーが返る") {
  const auto dir = make_temp_dir("glzdb_test_part_partial");
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto dbdir = dir / "dbdir";

  using Pdb = glzdb::database<State, glzdb::JsonPartitionedAdapter>;
  {
    auto db = Pdb::open(dbdir);
    REQUIRE(db);
    REQUIRE(db->insert(make_user(1, "alice")));
    REQUIRE(db->insert(Post{10, 1, "hello"}));
    REQUIRE(db->flush() == glzdb::error::none);
  }
  // Post.json を破損させる
  {
    std::ofstream f(dbdir / "Post.json", std::ios::trunc);
    f << "INVALID";
  }
  auto db = Pdb::open(dbdir);
  REQUIRE(!db);
  REQUIRE(db.error() == glzdb::error::parse_error);

  std::filesystem::remove_all(dir);
}

TEST_CASE("partitioned: 存在しないディレクトリからの open は空で成功") {
  const auto dir   = make_temp_dir("glzdb_test_part_empty");
  std::filesystem::remove_all(dir);
  // dir 自体を作らず dbdir も不存在の状態で open
  const auto dbdir = dir / "new_dbdir";
  REQUIRE(!std::filesystem::exists(dbdir));

  using Pdb = glzdb::database<State, glzdb::JsonPartitionedAdapter>;
  {
    auto db = Pdb::open(dbdir);
    REQUIRE(db);
    REQUIRE(db->count<User>() == 0);
    // flush でディレクトリが作成される
    REQUIRE(db->flush() == glzdb::error::none);
    REQUIRE(std::filesystem::exists(dbdir));
  }

  std::filesystem::remove_all(dir);
}

TEST_CASE("大量データの永続化 (Unified, 1000件)") {
  const auto dir = make_temp_dir("glzdb_test_bulk");
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto path = dir / "db.json";

  {
    auto db = Db::open(path);
    REQUIRE(db);
    for (uint64_t i = 1; i <= 1000; ++i) {
      REQUIRE(db->insert(User{i, "user" + std::to_string(i)}));
    }
    REQUIRE(db->count<User>() == 1000);
    REQUIRE(db->flush() == glzdb::error::none);
  }
  {
    auto db = Db::open(path);
    REQUIRE(db);
    REQUIRE(db->count<User>() == 1000);
    REQUIRE(db->get<User>(1)->name == "user1");
    REQUIRE(db->get<User>(500)->name == "user500");
    REQUIRE(db->get<User>(1000)->name == "user1000");
    // remove 後の永続化も確認
    REQUIRE(db->remove<User>(500));
    REQUIRE(db->flush() == glzdb::error::none);
  }
  {
    auto db = Db::open(path);
    REQUIRE(db);
    REQUIRE(db->count<User>() == 999);
    REQUIRE(db->get<User>(500) == nullptr);
  }

  std::filesystem::remove_all(dir);
}

TEST_CASE("大量データの永続化 (Partitioned, 500件)") {
  const auto dir   = make_temp_dir("glzdb_test_bulk_part");
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto dbdir = dir / "dbdir";

  using Pdb = glzdb::database<State, glzdb::JsonPartitionedAdapter>;
  {
    auto db = Pdb::open(dbdir);
    REQUIRE(db);
    for (uint64_t i = 1; i <= 500; ++i) {
      REQUIRE(db->insert(User{i, "u" + std::to_string(i)}));
      REQUIRE(db->insert(Post{i, i, "post" + std::to_string(i)}));
    }
    REQUIRE(db->count<User>() == 500);
    REQUIRE(db->count<Post>() == 500);
    REQUIRE(db->flush() == glzdb::error::none);
  }
  {
    auto db = Pdb::open(dbdir);
    REQUIRE(db);
    REQUIRE(db->count<User>() == 500);
    REQUIRE(db->count<Post>() == 500);
    REQUIRE(db->get<Post>(250)->title == "post250");
  }

  std::filesystem::remove_all(dir);
}

/// @brief ponytail スタイルのセルフチェック: テスト内の簡易 end-to-end デモ
TEST_CASE("demo: end-to-end") {
  const auto dir = make_temp_dir("glzdb_test_demo");
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);

  auto db = Db::open(dir / "db.json");
  REQUIRE(db);

  REQUIRE(db->insert(make_user(1, "alice")));
  REQUIRE(db->insert(User{2, "bob"}));
  REQUIRE(db->insert(Post{100, 1, "first post"}));
  REQUIRE(db->flush() == glzdb::error::none);

  {
    auto reopened = Db::open(dir / "db.json");
    REQUIRE(reopened);
    REQUIRE(reopened->count<User>() == 2);
    REQUIRE(reopened->count<Post>() == 1);

    auto alice_posts = reopened->related<&Post::user_id>(1);
    REQUIRE(alice_posts.size() == 1);
    REQUIRE(alice_posts[0]->title == "first post");
  }

  std::filesystem::remove_all(dir);
}
