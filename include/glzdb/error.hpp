#pragma once

#include <expected>
#include <string_view>

namespace glzdb {

/// @brief データベース操作で返されるエラーコード
enum class error {
  none,          ///< 成功 (sentinel)
  io_error,      ///< ファイルシステムの読み書き / rename 失敗
  duplicate_key, ///< insert() で id が既存
  not_found,     ///< update()/remove() で id が不存在
  parse_error,   ///< JSON パース失敗 (または型不一致)
  foreign_key_violation, ///< 外部キー制約違反
};

/// @brief 値または glzdb::error を搬載する expected 型
template <class T = void>
using result = std::expected<T, error>;

/// @brief エラーコードに対応する日本語メッセージを取得
/// @param e エラーコード
/// @return 対応するエラーメッセージ文字列
[[nodiscard]] constexpr std::string_view message(const error e) noexcept {
  using enum error;
  switch (e) {
  case none:
    return "エラーなし";
  case io_error:
    return "I/O エラー";
  case duplicate_key:
    return "重複キー";
  case not_found:
    return "未見つかり";
  case parse_error:
    return "パースエラー";
  case foreign_key_violation:
    return "外部キー制約違反";
  }
  return "不明なエラー";
}

}  // namespace glzdb
