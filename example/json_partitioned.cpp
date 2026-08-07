/// @file json_partitioned.cpp
/// @brief テーブル毎にファイルを分割して永続化する glzdb のデモ (JsonPartitionedAdapter)

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
using Db    = glzdb::database<State, glzdb::JsonPartitionedAdapter>;

int main() {
  const std::filesystem::path db_dir = "example_partitioned";

  {
    auto db = Db::open(db_dir);
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
    if (!db->insert(Post{10, 1, "hello"})) {
      std::cerr << "insert failed\n";
      return 1;
    }
    if (db->flush() != glzdb::error::none) {
      std::cerr << "flush failed\n";
      return 1;
    }

    std::cout << "wrote per-table files:\n";
    for (const auto& entry : std::filesystem::directory_iterator(db_dir)) {
      std::cout << "  " << entry.path().filename().string() << '\n';
    }
  }

  {
    auto db = Db::open(db_dir);
    if (!db) {
      std::cerr << "reopen failed: " << glzdb::message(db.error()) << '\n';
      return 1;
    }
    std::cout << "users: " << db->count<User>() << ", posts: " << db->count<Post>() << '\n';
    if (const auto* u = db->get<User>(1); u) {
      std::cout << "user 1: " << u->name << '\n';
    }
  }

  std::filesystem::remove_all(db_dir);
  return 0;
}
