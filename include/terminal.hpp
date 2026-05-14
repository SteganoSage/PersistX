#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// PersistX — Terminal Rendering Utilities
// ═══════════════════════════════════════════════════════════════════════════════
//
// ANSI color helpers, box-drawing, table rendering, and Windows VT100 setup.
// All output is UTF-8 + ANSI escape codes. On Windows 10+ this works natively
// after enabling Virtual Terminal Processing.
// ═══════════════════════════════════════════════════════════════════════════════

#include <string>
#include <vector>
#include <utility>
#include <cstdint>
#include "common.hpp"

namespace term {

// ─── platform setup ──────────────────────────────────────────────────────────
// Call once at program start. Enables ANSI escape codes on Windows and sets
// the console to UTF-8 mode.
void enable_ansi();

// ─── ANSI color wrappers ─────────────────────────────────────────────────────
// Each function wraps the input string in the appropriate escape codes and
// appends a reset at the end.

std::string red(const std::string& s);
std::string green(const std::string& s);
std::string yellow(const std::string& s);
std::string cyan(const std::string& s);
std::string magenta(const std::string& s);
std::string blue(const std::string& s);
std::string bold(const std::string& s);
std::string dim(const std::string& s);
std::string bold_cyan(const std::string& s);
std::string bold_green(const std::string& s);
std::string bold_red(const std::string& s);
std::string bold_yellow(const std::string& s);
std::string bold_magenta(const std::string& s);

// ─── box drawing ─────────────────────────────────────────────────────────────
// Draws a single box with a title and key-value rows.
//
//   ┌─── Title ──────────────┐
//   │ Key1:       Value1     │
//   │ Key2:       Value2     │
//   └────────────────────────┘

void print_box(const std::string& title,
               const std::vector<std::pair<std::string, std::string>>& rows,
               int width = 32);

// Draws two boxes side by side.
void print_two_boxes(
    const std::string& title1,
    const std::vector<std::pair<std::string, std::string>>& rows1,
    const std::string& title2,
    const std::vector<std::pair<std::string, std::string>>& rows2,
    int width = 32);

// ─── table rendering ────────────────────────────────────────────────────────
// Draws a table with headers and rows, auto-sizing columns.
//
//   ┌───────┬──────────┬───────┐
//   │ Col1  │  Col2    │ Col3  │
//   ├───────┼──────────┼───────┤
//   │ val   │  val     │ val   │
//   └───────┴──────────┴───────┘

void print_table(const std::vector<std::string>& headers,
                 const std::vector<std::vector<std::string>>& rows);

// ─── banner & prompt ─────────────────────────────────────────────────────────

void print_banner();
void print_prompt(bool in_txn, txn_id_t txn_id);

// ─── utility ─────────────────────────────────────────────────────────────────

// Pad or truncate string to exactly `width` characters.
std::string pad(const std::string& s, int width);

// Right-align a string in a field of `width` characters.
std::string rpad(const std::string& s, int width);

// Clear the terminal screen.
void clear_screen();

} // namespace term
