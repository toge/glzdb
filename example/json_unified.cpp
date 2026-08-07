/// @file json_unified.cpp
/// @brief 単一 JSON ファイルで永続化する glzdb のデモ (JsonAdapter)

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <tuple>

#include <glzdb.hpp>

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

int main() {
  const std::filesystem::path db_path = "example_unified.json";

  {
    auto db = Db::open(db_path);
    if (!db) {
      std::cerr << "open failed: " << glzdb::message(db.error()) << '\n';
      return 1;
    }

    if (!db->insert(User{1, "alice"})) {
      std::cerr << "insert failed\n";
      return 1;
    }
    if (!db->insert(User{2, "bob"})) {
      std::cerr << "insert failed\n";
      return 1;
    }
    if (!db->insert(Post{10, 1, "first post"})) {
      std::cerr << "insert failed\n";
      return 1;
    }
    if (!db->insert(Post{11, 1, "second post"})) {
      std::cerr << "insert failed\n";
      return 1;
    }
    if (!db->insert(Post{12, 2, "bob's post"})) {
      std::cerr << "insert failed\n";
      return 1;
    }
    if (db->flush() != glzdb::error::none) {
      std::cerr << "flush failed\n";
      return 1;
    }  // atomic 書き込み: 一時ファイル + rename

    std::cout << "inserted " << db->count<User>() << " users, " << db->count<Post>() << " posts\n";
  }  // デストラクタもフラッシュ

  {
    auto db = Db::open(db_path);
    if (!db) {
      std::cerr << "reopen failed: " << glzdb::message(db.error()) << '\n';
      return 1;
    }

    const auto* alice = db->get<User>(1);
    if (alice) {
      std::cout << "user 1: " << alice->name << '\n';
    }

    std::cout << "alice's posts:\n";
    for (const auto* p : db->related<&Post::user_id>(1)) {
      std::cout << "  - " << p->title << '\n';
    }
  }

  std::filesystem::remove(db_path);
  return 0;
}
