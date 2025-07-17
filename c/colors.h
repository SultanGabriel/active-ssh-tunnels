#ifndef COLORS_H
#define COLORS_H

// ANSI Color Codes für geile Terminal-Optik
#define C_RESET   "\033[0m"
#define C_BOLD    "\033[1m"
#define C_DIM     "\033[2m"
#define C_UNDERLINE "\033[4m"

// Standard Farben
#define C_RED     "\033[1;31m"
#define C_GREEN   "\033[1;32m"
#define C_YELLOW  "\033[1;33m"
#define C_BLUE    "\033[1;34m"
#define C_MAGENTA "\033[1;35m"
#define C_CYAN    "\033[1;36m"
#define C_WHITE   "\033[1;37m"
#define C_GREY    "\033[1;30m"

// Background Farben für Highlights
#define C_BG_RED     "\033[41m"
#define C_BG_GREEN   "\033[42m"
#define C_BG_YELLOW  "\033[43m"

// Status-spezifische Farben
#define C_SUCCESS    C_GREEN
#define C_ERROR      C_RED
#define C_WARNING    C_YELLOW
#define C_INFO       C_CYAN
#define C_STATUS     C_BLUE

// Tunnel Status Emojis/Symbole
#define SYMBOL_RUNNING    "🔗"
#define SYMBOL_STOPPED    "⭕"
#define SYMBOL_ERROR      "❌"
#define SYMBOL_STARTING   "🔄"
#define SYMBOL_RECONNECT  "⚡"
#define SYMBOL_ARROW      "➔"

// Fallback für Windows/simple terminals
#ifdef _WIN32
#undef SYMBOL_RUNNING
#undef SYMBOL_STOPPED
#undef SYMBOL_ERROR
#undef SYMBOL_STARTING
#undef SYMBOL_RECONNECT
#undef SYMBOL_ARROW
#define SYMBOL_RUNNING    "[ON]"
#define SYMBOL_STOPPED    "[OFF]"
#define SYMBOL_ERROR      "[ERR]"
#define SYMBOL_STARTING   "[...]"
#define SYMBOL_RECONNECT  "[REC]"
#define SYMBOL_ARROW      "->"
#endif

#endif // COLORS_H
