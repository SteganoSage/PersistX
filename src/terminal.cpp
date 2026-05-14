#include "terminal.hpp"
#include <iostream>
#include <algorithm>
#include <sstream>
#include <iomanip>

#ifdef _WIN32
#include <windows.h>
#endif

namespace term {

// ─── platform setup ──────────────────────────────────────────────────────────

void enable_ansi() {
#ifdef _WIN32
    // Enable Virtual Terminal Processing for ANSI escape codes on Windows 10+.
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE) {
        DWORD mode = 0;
        if (GetConsoleMode(hOut, &mode)) {
            mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            SetConsoleMode(hOut, mode);
        }
    }
    // Switch console to UTF-8 so box-drawing characters render correctly.
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
#endif
    // On Linux/macOS, terminals support ANSI natively — nothing to do.
}

// ─── ANSI escape helpers ─────────────────────────────────────────────────────

static const char* RESET   = "\033[0m";
static const char* BOLD    = "\033[1m";
static const char* DIM_SEQ = "\033[2m";

std::string red(const std::string& s)          { return std::string("\033[31m") + s + RESET; }
std::string green(const std::string& s)        { return std::string("\033[32m") + s + RESET; }
std::string yellow(const std::string& s)       { return std::string("\033[33m") + s + RESET; }
std::string blue(const std::string& s)         { return std::string("\033[34m") + s + RESET; }
std::string magenta(const std::string& s)      { return std::string("\033[35m") + s + RESET; }
std::string cyan(const std::string& s)         { return std::string("\033[36m") + s + RESET; }
std::string bold(const std::string& s)         { return std::string(BOLD) + s + RESET; }
std::string dim(const std::string& s)          { return std::string(DIM_SEQ) + s + RESET; }
std::string bold_cyan(const std::string& s)    { return std::string("\033[1;36m") + s + RESET; }
std::string bold_green(const std::string& s)   { return std::string("\033[1;32m") + s + RESET; }
std::string bold_red(const std::string& s)     { return std::string("\033[1;31m") + s + RESET; }
std::string bold_yellow(const std::string& s)  { return std::string("\033[1;33m") + s + RESET; }
std::string bold_magenta(const std::string& s) { return std::string("\033[1;35m") + s + RESET; }

// ─── string utilities ────────────────────────────────────────────────────────

std::string pad(const std::string& s, int width) {
    if (static_cast<int>(s.size()) >= width) return s.substr(0, static_cast<size_t>(width));
    return s + std::string(static_cast<size_t>(width) - s.size(), ' ');
}

std::string rpad(const std::string& s, int width) {
    if (static_cast<int>(s.size()) >= width) return s.substr(0, static_cast<size_t>(width));
    return std::string(static_cast<size_t>(width) - s.size(), ' ') + s;
}

// ─── box drawing ─────────────────────────────────────────────────────────────

void print_box(const std::string& title,
               const std::vector<std::pair<std::string, std::string>>& rows,
               int width) {
    // Ensure minimum width to fit title
    int title_len = static_cast<int>(title.size());
    if (width < title_len + 6) width = title_len + 6;

    int inner = width - 2;  // content width inside the box (excluding │ │)

    // ┌─── Title ──────────┐
    std::cout << "  " << cyan("\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80 ")
              << bold_cyan(title) << " ";
    int dashes_after = inner - title_len - 4;
    if (dashes_after < 1) dashes_after = 1;
    for (int i = 0; i < dashes_after; ++i) std::cout << cyan("\xe2\x94\x80");
    std::cout << cyan("\xe2\x94\x90") << "\n";

    // │ Key:   Value   │
    for (auto& [key, val] : rows) {
        std::string line = " " + key;
        int val_start = inner / 2;  // align values at roughly the halfway point
        int spaces = val_start - static_cast<int>(line.size());
        if (spaces < 1) spaces = 1;
        line += std::string(static_cast<size_t>(spaces), ' ') + val;

        // Pad to fill the box width
        if (static_cast<int>(line.size()) < inner)
            line += std::string(static_cast<size_t>(inner) - line.size(), ' ');
        else
            line = line.substr(0, static_cast<size_t>(inner));

        std::cout << "  " << cyan("\xe2\x94\x82")
                  << dim(line)
                  << cyan("\xe2\x94\x82") << "\n";
    }

    // └────────────────────┘
    std::cout << "  " << cyan("\xe2\x94\x94");
    for (int i = 0; i < inner; ++i) std::cout << cyan("\xe2\x94\x80");
    std::cout << cyan("\xe2\x94\x98") << "\n";
}

void print_two_boxes(
    const std::string& title1,
    const std::vector<std::pair<std::string, std::string>>& rows1,
    const std::string& title2,
    const std::vector<std::pair<std::string, std::string>>& rows2,
    int width) {

    int inner = width - 2;

    // Lambda to build lines for a single box
    auto build_box_lines = [&](const std::string& title,
                               const std::vector<std::pair<std::string, std::string>>& rows)
        -> std::vector<std::string>
    {
        std::vector<std::string> lines;

        // Top border with title
        int title_len = static_cast<int>(title.size());
        std::string top = cyan("\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80 ")
                        + bold_cyan(title) + " ";
        int dashes = inner - title_len - 4;
        if (dashes < 1) dashes = 1;
        for (int i = 0; i < dashes; ++i) top += cyan("\xe2\x94\x80");
        top += cyan("\xe2\x94\x90");
        lines.push_back(top);

        // Content rows
        for (auto& [key, val] : rows) {
            std::string line = " " + key;
            int val_start = inner / 2;
            int spaces = val_start - static_cast<int>(line.size());
            if (spaces < 1) spaces = 1;
            line += std::string(static_cast<size_t>(spaces), ' ') + val;
            if (static_cast<int>(line.size()) < inner)
                line += std::string(static_cast<size_t>(inner) - line.size(), ' ');
            else
                line = line.substr(0, static_cast<size_t>(inner));

            lines.push_back(cyan("\xe2\x94\x82") + dim(line) + cyan("\xe2\x94\x82"));
        }

        // Bottom border
        std::string bot = cyan("\xe2\x94\x94");
        for (int i = 0; i < inner; ++i) bot += cyan("\xe2\x94\x80");
        bot += cyan("\xe2\x94\x98");
        lines.push_back(bot);

        return lines;
    };

    auto left  = build_box_lines(title1, rows1);
    auto right = build_box_lines(title2, rows2);

    // Pad to same height
    size_t max_lines = std::max(left.size(), right.size());
    while (left.size()  < max_lines) left.push_back(std::string(static_cast<size_t>(width), ' '));
    while (right.size() < max_lines) right.push_back(std::string(static_cast<size_t>(width), ' '));

    for (size_t i = 0; i < max_lines; ++i) {
        std::cout << "  " << left[i] << "  " << right[i] << "\n";
    }
}

// ─── table rendering ────────────────────────────────────────────────────────

void print_table(const std::vector<std::string>& headers,
                 const std::vector<std::vector<std::string>>& rows) {
    if (headers.empty()) return;

    size_t cols = headers.size();

    // Compute column widths
    std::vector<int> widths(cols, 0);
    for (size_t c = 0; c < cols; ++c) {
        widths[c] = static_cast<int>(headers[c].size());
    }
    for (auto& row : rows) {
        for (size_t c = 0; c < cols && c < row.size(); ++c) {
            widths[c] = std::max(widths[c], static_cast<int>(row[c].size()));
        }
    }
    // Add padding
    for (auto& w : widths) w += 2;

    // Helper: horizontal line
    auto hline = [&](const std::string& left_ch, const std::string& mid_ch,
                     const std::string& right_ch) {
        std::cout << "  " << cyan(left_ch);
        for (size_t c = 0; c < cols; ++c) {
            for (int i = 0; i < widths[c]; ++i) std::cout << cyan("\xe2\x94\x80");
            if (c + 1 < cols) std::cout << cyan(mid_ch);
        }
        std::cout << cyan(right_ch) << "\n";
    };

    // Top border
    hline("\xe2\x94\x8c", "\xe2\x94\xac", "\xe2\x94\x90");

    // Header row
    std::cout << "  " << cyan("\xe2\x94\x82");
    for (size_t c = 0; c < cols; ++c) {
        std::string cell = " " + headers[c];
        cell = pad(cell, widths[c]);
        std::cout << bold_cyan(cell) << cyan("\xe2\x94\x82");
    }
    std::cout << "\n";

    // Separator
    hline("\xe2\x94\x9c", "\xe2\x94\xbc", "\xe2\x94\xa4");

    // Data rows
    for (auto& row : rows) {
        std::cout << "  " << cyan("\xe2\x94\x82");
        for (size_t c = 0; c < cols; ++c) {
            std::string val = c < row.size() ? row[c] : "";
            std::string cell = " " + val;
            cell = pad(cell, widths[c]);
            std::cout << cell << cyan("\xe2\x94\x82");
        }
        std::cout << "\n";
    }

    // Bottom border
    hline("\xe2\x94\x94", "\xe2\x94\xb4", "\xe2\x94\x98");
}

// ─── banner ──────────────────────────────────────────────────────────────────

void print_banner() {
    std::cout << "\n";
    std::cout << "  " << bold_cyan("\xe2\x95\x94\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x97") << "\n";
    std::cout << "  " << bold_cyan("\xe2\x95\x91") << "                                                                " << bold_cyan("\xe2\x95\x91") << "\n";
    std::cout << "  " << bold_cyan("\xe2\x95\x91") << bold("       \xe2\x97\x86  P E R S I S T X   S t o r a g e   E n g i n e  \xe2\x97\x86      ") << bold_cyan("\xe2\x95\x91") << "\n";
    std::cout << "  " << bold_cyan("\xe2\x95\x91") << dim("                  v0.1.0  |  ACID  |  ARIES  |  B+Tree            ") << bold_cyan("\xe2\x95\x91") << "\n";
    std::cout << "  " << bold_cyan("\xe2\x95\x91") << "                                                                " << bold_cyan("\xe2\x95\x91") << "\n";
    std::cout << "  " << bold_cyan("\xe2\x95\x9a\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x9d") << "\n";
    std::cout << "\n";
    std::cout << "  " << dim("Type ") << bold("help") << dim(" for available commands.\n");
    std::cout << "\n";
}

// ─── prompt ──────────────────────────────────────────────────────────────────

void print_prompt(bool in_txn, txn_id_t txn_id) {
    if (in_txn) {
        std::cout << "  " << yellow("persistx")
                  << dim("[") << bold_yellow("txn#" + std::to_string(txn_id)) << dim("]")
                  << bold_cyan("> ");
    } else {
        std::cout << "  " << cyan("persistx") << bold_cyan("> ");
    }
    std::cout.flush();
}

// ─── clear screen ────────────────────────────────────────────────────────────

void clear_screen() {
    std::cout << "\033[2J\033[H";
    std::cout.flush();
}

} // namespace term
