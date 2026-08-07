#pragma once

/// @file glzdb.hpp
/// @brief glzdb: glaze ベースのヘッダーオンリー JSON データベース
///
/// 小規模・シングルプロセス・インメモリアプリ向け。モデルは構造体に `id` メンバを
/// 定義し、state タプルに `table<T>` を並べると ORM 的な CRUD / リレーション操作が
/// 利用可能になる。永続化はアダプタ (JsonAdapter / JsonPartitionedAdapter) に
/// 委譲し、atomic書き込み (一時ファイル + rename) を保証する。

#include "glzdb/adapters.hpp"
#include "glzdb/database.hpp"
#include "glzdb/error.hpp"
#include "glzdb/table.hpp"
#include "glzdb/traits.hpp"
