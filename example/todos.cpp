/// @file todos.cpp
/// @brief glzdb バックエンドの簡易 ToDo CLI

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <system_error>
#include <tuple>

#include <glzdb.hpp>

struct Todo {
  uint64_t    id;
  std::string title;
  bool        done;
};

using State = std::tuple<glzdb::table<Todo>>;
using Db    = glzdb::database<State, glzdb::JsonAdapter>;

namespace {
/// @brief 全 ToDo を一覧表示
void list(Db& db) {
  std::cout << "todos (" << db.count<Todo>() << "):\n";
  for (const auto& t : db.get_all<Todo>()) {
    std::cout << "  [" << (t.done ? 'x' : ' ') << "] #" << t.id << " " << t.title << '\n';
  }
}

/// @brief 文字列を uint64_t の id にパース (失敗時は nullopt)
std::optional<uint64_t> parse_id(const std::string& s) {
  uint64_t value{};
  const auto res = std::from_chars(s.data(), s.data() + s.size(), value);
  if (res.ec != std::errc{} || res.ptr != s.data() + s.size()) {
    return std::nullopt;
  }
  return value;
}
}  // namespace

int main() {
  const std::filesystem::path db_path = "todos.json";

  auto db = Db::open(db_path);
  if (!db) {
    std::cerr << "open failed: " << glzdb::message(db.error()) << '\n';
    return 1;
  }

  // 新規データベースの場合は初期データを 2 件挿入
  if (db->count<Todo>() == 0) {
    if (!db->insert(Todo{1, "write glzdb", true}) || !db->insert(Todo{2, "eat lunch", false})) {
      std::cerr << "insert failed\n";
      return 1;
    }
    if (db->flush() != glzdb::error::none) {
      std::cerr << "flush failed\n";
      return 1;
    }
  }

  list(*db);

  std::cout << "commands: list | add <title> | done <id> | rm <id> | quit\n";

  std::string line;
  while (std::cout << "> " && std::getline(std::cin, line)) {
    if (line == "quit" || line == "exit") {
      break;
    }
    if (line == "list") {
      list(*db);
      continue;
    }

    if (line.rfind("add ", 0) == 0) {
      const auto title = line.substr(4);
      if (title.empty()) {
        std::cout << "usage: add <title>\n";
        continue;
      }
      // v1 では明示的な id を使用
      // ponytail: O(n) max+1 scan, fine for CLI; 性能が必要なら next_id カウンタを永続化
      uint64_t next = 1;
      for (const auto& t : db->get_all<Todo>()) {
        next = std::max(next, t.id + 1);
      }
      if (!db->insert(Todo{next, title, false})) {
        std::cout << "insert failed\n";
        continue;
      }
      if (db->flush() != glzdb::error::none) {
        std::cout << "flush failed\n";
      }
      std::cout << "added #" << next << '\n';
      continue;
    }

    if (line.rfind("done ", 0) == 0) {
      const auto  id = parse_id(line.substr(5));
      const auto* t  = id ? db->get<Todo>(*id) : nullptr;
      if (!t) {
        std::cout << "no such todo\n";
        continue;
      }
      auto updated = *t;
      updated.done = true;
      if (!db->update(updated)) {
        std::cout << "update failed\n";
        continue;
      }
      if (db->flush() != glzdb::error::none) {
        std::cout << "flush failed\n";
      }
      std::cout << "done #" << *id << '\n';
      continue;
    }

    if (line.rfind("rm ", 0) == 0) {
      const auto id = parse_id(line.substr(3));
      if (id && db->remove<Todo>(*id)) {
        if (db->flush() != glzdb::error::none) {
          std::cout << "flush failed\n";
        }
        std::cout << "removed #" << *id << '\n';
      } else {
        std::cout << "no such todo\n";
      }
      continue;
    }

    std::cout << "unknown command\n";
  }

  return 0;
}
