#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace llmlog::core {

/// Splits an isql-style SQL script into individual statements.
///
/// Handles:
///   * 'SET TERM X ;' / 'SET TERM ; X' directives — swaps the active
///     terminator for subsequent statements. SET TERM itself is consumed
///     and never emitted as a statement.
///   * Line comments starting with '--' (until end of line).
///   * Block comments '/* ... */' (non-nested, Firebird-style).
///   * Single-quoted string literals, including doubled '' escapes.
///   * Case-insensitive matching of SET TERM and keywords.
///
/// Does NOT handle nested block comments (Firebird does not support them).
/// Does NOT perform any SQL parsing beyond terminator detection — statements
/// containing BEGIN/END blocks must use a non-';' terminator via SET TERM.
///
/// The returned statements are trimmed of leading/trailing whitespace and
/// have the terminator stripped. Empty fragments are never returned.
std::vector<std::string> splitIsqlScript(std::string_view script);

} // namespace llmlog::core
