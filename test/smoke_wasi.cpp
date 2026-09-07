/// @file smoke_wasi.cpp
/// @brief WASI スモーク: Catch2 なしで CRUD + 永続化ラウンドトリップを最小確認
///
/// wasm32-wasip1/wasip2 では Catch2 がビルドできないため素の main を使用。
/// wasmtime 実行時はカレントを --dir で preopen すること
/// (相対パス smoke_wasi.json に読み書きするため)。

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <tuple>

#include <glzdb.hpp>

namespace glzdb_wasi_smoke {

struct User {
  uint64_t    id;
  std::string name;
};

struct Post {
  uint64_t    id;
  uint64_t    user_id;
  std::string title;
};

}  // namespace glzdb_wasi_smoke

namespace {

using glzdb_wasi_smoke::Post;
using glzdb_wasi_smoke::User;
using State = std::tuple<glzdb::table<glzdb_wasi_smoke::User>, glzdb::table<glzdb_wasi_smoke::Post>>;
using Db    = glzdb::database<State, glzdb::JsonAdapter>;

int fail(const char* msg) {
  std::puts(msg);
  return 1;
}

}  // namespace

int main() {
  const std::filesystem::path db_path = "smoke_wasi.json";
  std::error_code             ec;
  std::filesystem::remove(db_path, ec);

  {
    auto db = Db::open(db_path);
    if (!db) {
      return fail("open failed");
    }
    if (!db->insert(User{1, "alice"})) {
      return fail("insert user failed");
    }
    if (!db->insert(Post{10, 1, "hello"})) {
      return fail("insert post failed");
    }
    if (db->flush() != glzdb::error::none) {
      return fail("flush failed");
    }
    if (db->count<User>() != 1 || db->count<Post>() != 1) {
      return fail("count mismatch");
    }
  }

  {
    auto db = Db::open(db_path);
    if (!db) {
      return fail("reopen failed");
    }
    const auto* u = db->get<User>(1);
    if (u == nullptr || u->name != "alice") {
      return fail("get mismatch");
    }
    auto posts = db->related<&Post::user_id>(1);
    if (posts.size() != 1 || posts[0]->title != "hello") {
      return fail("related mismatch");
    }
  }

  std::filesystem::remove(db_path, ec);
  std::puts("smoke_wasi: OK");
  return 0;
}
