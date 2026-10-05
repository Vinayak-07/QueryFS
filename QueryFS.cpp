#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#else
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <csignal>
#include <cstdlib>
#endif

// ============================================================================
// QueryFS v2.0 — interactive file search engine
//
// A C++17 file search engine with a hand-rolled terminal UI. Indexes a
// directory tree ONCE into an in-memory singly linked list, then answers
// queries against that list — no disk access between queries.
//
// Structure of this file:
//   1. Platform includes + global switches
//   2. Terminal class   — raw keypress input, size, clear (encapsulated,
//                         two implementations behind #ifdef)
//   3. Text utilities   — UTF-8 safe truncation, case folding, human sizes
//   4. FileNode/FileList— the index (encapsulation, composition, O(1) append)
//   5. SearchStrategy   — abstract interface + 3 concrete strategies
//   6. Indexer          — directory walk -> FileList, with a background thread
//   7. Cache            — persist the index so we don't re-scan every run
//   8. Query            — /name /ext /dir parser + narrowing optimization
//   9. Renderer         — builds the whole frame as one string, writes once
//  10. main
//
// Compile:
//   Linux/macOS :  g++ -std=c++17 -O2 QueryFS.cpp -o queryfs
//   MSYS2       :  g++ -std=c++17 -O2 QueryFS.cpp -o queryfs.exe
//   (older GCC 8 needs -lstdc++fs at the end)
//
// Run:   ./queryfs [path]      (path optional; defaults to $HOME / C:\)
// ============================================================================

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// 1. Global switches
//
// One place decides whether we may emit escape sequences. Four inputs feed it:
//   --no-color        command line flag from the user
//   NO_COLOR          env var, the cross-platform standard (any value = off)
//   non-tty stdout    piping to a file must never receive escape codes
//   TERM=dumb         a terminal that cannot render them
// Everything downstream asks Color::on() rather than re-deciding, so the rules
// live in exactly one spot.
// ---------------------------------------------------------------------------
namespace Color {
bool enabled = true;

void init(bool flagNoColor) {
    const char* nc = std::getenv("NO_COLOR");
    const char* term = std::getenv("TERM");
    enabled = !flagNoColor
              && (nc == nullptr)                      // NO_COLOR unset
              && (term == nullptr || std::strcmp(term, "dumb") != 0);
}
inline bool on() { return enabled; }

// SGR helpers. 256-colour form (\x1b[38;5;Nm) because it gives a smooth
// cyan -> magenta gradient for the banner, which 16 colours cannot do.
inline std::string fg(int n) { return "\x1b[38;5;" + std::to_string(n) + "m"; }
inline std::string reset()  { return "\x1b[0m"; }
inline std::string bold()   { return "\x1b[1m"; }
inline std::string inverse(){ return "\x1b[7m"; }
inline std::string dim()    { return "\x1b[2m"; }

inline std::string paint(int colour, const std::string& text) {
    if (!enabled) return text;
    return fg(colour) + text + reset();
}
} // namespace Color

// Does the terminal speak UTF-8? Decides box-drawing vs ASCII fallback.
// We do not ask the OS to change the locale: the checks below are cheap and
// cannot fail, unlike setlocale().
static bool g_utf8 = false;

// ---------------------------------------------------------------------------
// 2. Terminal — raw keypress input and screen geometry, platform-abstracted.
//
// Why a class instead of loose functions: raw mode is GLOBAL terminal state.
// If raw mode is on and the program exits, the user's shell is left without
// echo and without line buffering — an unusable shell. So the enabling and the
// restoring must be paired, and the pairing must survive every exit path.
// Three mechanisms guarantee that here:
//     (a) RAII destructor  — normal return and stack unwinding
//     (b) atexit() handler — for paths that bypass the destructor
//     (c) SIGINT handler   — the user pressing Ctrl+C at the keyboard
// All three call the same idempotent restore(), which is safe to call twice.
//
// Two implementations, one interface:
//     _WIN32 : SetConsoleMode + ReadConsoleInputW, plus
//              ENABLE_VIRTUAL_TERMINAL_PROCESSING so ANSI escapes render,
//              plus SetConsoleOutputCP(CP_UTF8) so our box-drawing bytes
//              are not mangled into '?' by the code page.
//     POSIX  : termios raw mode + read(), parsing the escape sequences
//              that a terminal sends for arrow keys.
// ---------------------------------------------------------------------------

// What a keypress is. Char carries `ch`; the rest are self-describing.
enum class KeyType {
    Char, Backspace, Enter, Esc,
    Up, Down, Left, Right,
    PageUp, PageDown, Home, End, Delete,
    CtrlC, Eof, None
};

struct Key {
    KeyType type = KeyType::None;
    char ch = '\0';
};

class Terminal {
public:
    Terminal() = default;
    ~Terminal() { restore(); }          // (a) RAII
    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;

    // True when stdin is an interactive terminal. When it is not (input piped
    // or redirected) the program must fall back to line-based mode instead of
    // blocking forever in raw mode on a pipe that will never deliver a key.
    static bool stdinIsTty();

    // Turns raw mode on. Safe to call twice.
    bool enterRawMode();

    // Puts the terminal back the way we found it. Idempotent.
    void restore();

    // Blocks until one key is available. Returns {KeyType::Eof} on EOF.
    Key readKey();

    // Screen geometry, re-read on every call: the user can resize the window
    // mid-search, and a cached size would corrupt the layout.
    int getWidth()  const;
    int getHeight() const;

    // Clears the screen and homes the cursor.
    void clear();

    // Is the terminal able to render UTF-8 box-drawing characters?
    bool utf8() const { return g_utf8; }

private:
    bool raw_ = false;
    bool restored_ = true;   // starts "restored" so a failed enterRawMode()
                            // does not try to restore something never changed

    // POSIX-only saved state, and the pointer the signal handler needs.
    struct Impl;
    static void restoreSignalHandler(int sig);
};

// --- POSIX terminal state, kept out of the class so <termios.h> details do
// --- not leak into the header section above.
#ifndef _WIN32
namespace {
struct PosixTerm {
    struct termios saved{};
    bool valid = false;
    volatile sig_atomic_t active = 0;   // read by the signal handler
};
PosixTerm g_term;

PosixTerm& termState() { return g_term; }

// Restoring is two steps: put the old termios struct back, then clear the flag
// saying "we changed something". The flag is cleared FIRST so that a signal
// arriving mid-restore cannot start a second, interleaved restore.
void posixRestore() {
    PosixTerm& t = termState();
    if (!t.valid) return;
    t.valid = false;
    t.active = 0;
    ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &t.saved);
}
} // namespace

// The one thing that must never happen here is dying by signal: a handler
// that re-raises SIGINT leaves the process with exit status 128+2, so the
// orderly shutdown in runInteractive() — cache write, cursor restore, goodbye
// message — is skipped entirely, and the terminal is left wherever the signal
// found it.
//
// Raw mode has ISIG CLEARED, so Ctrl+C is delivered to us as an ordinary
// 0x03 byte and never reaches this handler. The handler exists purely as a
// safety net for SIGTERM and for Ctrl+C arriving while the process is NOT in
// raw mode (for example during the cache load, before enterRawMode).
//
// So: restore the terminal, then leave. Exiting with 130 is the conventional
// status for "terminated by SIGINT" and keeps the exit code honest.
void Terminal::restoreSignalHandler(int sig) {
    posixRestore();
    std::_Exit(128 + sig);
}

bool Terminal::stdinIsTty() { return ::isatty(STDIN_FILENO) == 1; }

bool Terminal::enterRawMode() {
    if (raw_) return true;
    PosixTerm& t = termState();

    // CFGGETATTR / CFGSETATTR: a *local* copy is modified and written back, so
    // the user's actual terminal keeps whatever else was configured. We only
    // clear ISIG because we want Ctrl+C to arrive as a KEY, not as SIGINT —
    // that way the app can shut down in an orderly way and repaint.
    if (::tcgetattr(STDIN_FILENO, &t.saved) != 0) return false;
    struct termios raw = t.saved;

    // ISIG MUST be cleared as well. While it is set, Ctrl+C is consumed by the
    // kernel and delivered as SIGINT — the process is killed before read()
    // ever returns, so the app cannot shut down in an orderly way and the
    // 0x03 byte never arrives. Clearing ISIG hands Ctrl+C to us as an ordinary
    // character, which readKey() reports as KeyType::CtrlC and the main loop
    // handles like Esc. That is also why we install our own SIGINT handler
    // below: it is the safety net for a signal that arrives outside raw mode.
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);  // no echo, no line buffering
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_oflag &= ~(OPOST);                    // we do our own \n placement
    raw.c_cflag |= (CS8);

    // VMIN=0 VTIME=1: a read() returns after 100ms with nothing rather than
    // blocking forever. This is what lets a bare Esc be distinguished from the
    // start of an arrow-key escape sequence.
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (::tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) return false;

    t.valid = true;
    t.active = 1;
    raw_ = true;
    restored_ = false;

    // (b) covers exit paths that never touch the destructor.
    std::atexit(posixRestore);
    // (c) covers the user pressing Ctrl+C. SA_RESTART is deliberately NOT
    // set: we want the interrupted read() to return so the loop can exit.
    struct sigaction sa{};
    sa.sa_handler = Terminal::restoreSignalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    ::sigaction(SIGINT, &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);
    return true;
}

void Terminal::restore() {
    if (restored_) return;
    posixRestore();
    restored_ = true;
    raw_ = false;
}

Key Terminal::readKey() {
    unsigned char c = 0;
    const ssize_t n = ::read(STDIN_FILENO, &c, 1);

    if (n <= 0) {
        // A zero-length read with VMIN=0/VTIME=1 means "timed out, no key" —
        // return None so the caller can keep redrawing (spinner animation).
        if (n == 0) return Key{KeyType::None, '\0'};
        return Key{KeyType::Eof, '\0'};
    }

    switch (c) {
        case 0x03: return Key{KeyType::CtrlC, '\0'};   // ETX
        case 0x7F: return Key{KeyType::Backspace, '\0'};
        case '\r': case '\n': return Key{KeyType::Enter, '\0'};
        case 0x1B: break;                                // ESC: maybe a sequence
        default:   return Key{KeyType::Char, static_cast<char>(c)};
    }

    // ESC seen. Peek with a short timeout: if nothing follows, the user really
    // pressed Esc; if bytes follow, they are a CSI/SS3 sequence.
    //
    // The peek must stay in RAW mode with a short VMIN/VTIME. Deriving it from
    // `saved` would put the terminal back into canonical mode, where read()
    // blocks until a whole line arrives — so a bare Esc would never be seen
    // and the program would look frozen.
    struct termios peek{};
    ::tcgetattr(STDIN_FILENO, &peek);
    peek.c_lflag &= ~(ECHO | ICANON);
    peek.c_cc[VMIN] = 0;
    peek.c_cc[VTIME] = 1;
    ::tcsetattr(STDIN_FILENO, TCSANOW, &peek);

    unsigned char b = 0;
    if (::read(STDIN_FILENO, &b, 1) != 1) {
        // Nothing followed within the timeout: it really was a bare Esc.
        return Key{KeyType::Esc, '\0'};
    }

    Key out{KeyType::None, '\0'};
    if (b == '[') {
        // CSI: parameter bytes then a final byte in @..~
        unsigned char seq[8];
        int nseq = 0;
        while (nseq < 6) {
            unsigned char p = 0;
            if (::read(STDIN_FILENO, &p, 1) != 1) break;
            if (p >= 0x40 && p <= 0x7E) {
                switch (p) {
                    case 'A': out.type = KeyType::Up; break;
                    case 'B': out.type = KeyType::Down; break;
                    case 'C': out.type = KeyType::Right; break;
                    case 'D': out.type = KeyType::Left; break;
                    case 'H': out.type = KeyType::Home; break;
                    case 'F': out.type = KeyType::End; break;
                    case '~': {
                        // [1~ Home  [2~ Insert  [3~ Delete  [4~ End
                        // [5~ PgUp [6~ PgDn    [7~ Home   [8~ End
                        // nseq alone is ambiguous here — [3~, [5~ and [6~ all
                        // leave exactly one parameter byte — so switch on the
                        // parameter VALUE, not on how many there were.
                        const unsigned param = (nseq >= 1) ? seq[0] : '0';
                        switch (param) {
                            case '1': out.type = KeyType::Home; break;
                            case '2': out.type = KeyType::Delete; break;
                            case '3': out.type = KeyType::Delete; break;
                            case '4': out.type = KeyType::End; break;
                            case '5': out.type = KeyType::PageUp; break;
                            case '6': out.type = KeyType::PageDown; break;
                            case '7': out.type = KeyType::Home; break;
                            case '8': out.type = KeyType::End; break;
                            default: out.type = KeyType::None;
                        }
                        break;
                    }
                    default: out.type = KeyType::None;
                }
                break;
            }
            seq[nseq++] = p;
        }
    } else if (b == 'O') {
        // SS3, sent by some terminals for arrows in application mode.
        unsigned char f = 0;
        if (::read(STDIN_FILENO, &f, 1) == 1) {
            if (f == 'A') out.type = KeyType::Up;
            else if (f == 'B') out.type = KeyType::Down;
            else if (f == 'C') out.type = KeyType::Right;
            else if (f == 'D') out.type = KeyType::Left;
            else if (f == 'H') out.type = KeyType::Home;
            else if (f == 'F') out.type = KeyType::End;
        }
    }

    return out;   // raw mode with the short timeout is left in place
}

int Terminal::getWidth() const {
    struct winsize ws{};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return 80;   // sane fallback when output is not a terminal
}

int Terminal::getHeight() const {
    struct winsize ws{};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0)
        return ws.ws_row;
    return 24;
}

void Terminal::clear() {
    if (!Color::on()) return;
    std::fputs("\x1b[2J\x1b[H", stdout);
    std::fflush(stdout);
}

#else  // ======================= _WIN32 ======================================

bool Terminal::stdinIsTty() { return ::_isatty(_fileno(stdin)) != 0; }

bool Terminal::enterRawMode() {
    if (raw_) return true;

    HANDLE hIn = ::GetStdHandle(STD_INPUT_HANDLE);
    HANDLE hOut = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (hIn == INVALID_HANDLE_VALUE || hOut == INVALID_HANDLE_VALUE) return false;

    // Code page: without this the UTF-8 bytes we print (box drawing, the block
    // logo) are re-interpreted in the OEM code page and come out as '?'.
    ::SetConsoleOutputCP(CP_UTF8);
    ::SetConsoleCP(CP_UTF8);

    DWORD outMode = 0;
    if (::GetConsoleMode(hOut, &outMode))
        // ENABLE_VIRTUAL_TERMINAL_PROCESSING is what makes \x1b[38;5;Nm work
        // on a stock Windows console. Without it, 256-colour output is
        // printed as literal escape garbage.
        ::SetConsoleMode(hOut, outMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    DWORD inMode = 0;
    if (!::GetConsoleMode(hIn, &inMode)) return false;
    inMode &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT);
    inMode |= ENABLE_EXTENDED_FLAGS;   // needed for arrow keys / Ctrl+C as keys
    if (!::SetConsoleMode(hIn, inMode)) return false;

    raw_ = true;
    restored_ = false;
    return true;
}

void Terminal::restore() {
    if (restored_) return;
    restored_ = true;
    raw_ = false;
    // ENABLE_ECHO_INPUT / ENABLE_LINE_INPUT are the console's defaults, so
    // restoring means putting them back and turning processing back on.
    HANDLE hIn = ::GetStdHandle(STD_INPUT_HANDLE);
    if (hIn != INVALID_HANDLE_VALUE) {
        DWORD m = 0;
        if (::GetConsoleMode(hIn, &m))
            ::SetConsoleMode(hIn, (m & ~(ENABLE_EXTENDED_FLAGS))
                                  | ENABLE_ECHO_INPUT
                                  | ENABLE_LINE_INPUT
                                  | ENABLE_PROCESSED_INPUT);
    }
    ::SetConsoleOutputCP(::GetACP());
    ::SetConsoleCP(::GetACP());
}

Key Terminal::readKey() {
    HANDLE hIn = ::GetStdHandle(STD_INPUT_HANDLE);

    for (;;) {
        INPUT_RECORD rec{};
        DWORD got = 0;
        if (!::ReadConsoleInputW(hIn, &rec, 1, &got) || got == 0)
            return Key{KeyType::Eof, '\0'};

        if (rec.EventType != KEY_EVENT) continue;   // mouse/resize: ignore
        if (!rec.Event.KeyEvent.bKeyDown) continue; // ignore key-up

        switch (rec.Event.KeyEvent.wVirtualKeyCode) {
            case VK_ESCAPE:  return Key{KeyType::Esc, '\0'};
            case VK_RETURN:  return Key{KeyType::Enter, '\0'};
            case VK_BACK:    return Key{KeyType::Backspace, '\0'};
            case VK_UP:      return Key{KeyType::Up, '\0'};
            case VK_DOWN:    return Key{KeyType::Down, '\0'};
            case VK_LEFT:    return Key{KeyType::Left, '\0'};
            case VK_RIGHT:   return Key{KeyType::Right, '\0'};
            case VK_PRIOR:   return Key{KeyType::PageUp, '\0'};
            case VK_NEXT:    return Key{KeyType::PageDown, '\0'};
            case VK_HOME:    return Key{KeyType::Home, '\0'};
            case VK_END:     return Key{KeyType::End, '\0'};
            case VK_CONTROL:
                // Ctrl+C shows up as CONTROL held with 'C' pressed, because
                // ENABLE_PROCESSED_INPUT is off.
                if (rec.Event.KeyEvent.uChar.UnicodeChar == L'c' ||
                    rec.Event.KeyEvent.uChar.UnicodeChar == L'C')
                    return Key{KeyType::CtrlC, '\0'};
                continue;
            default: break;
        }

        const wchar_t wc = rec.Event.KeyEvent.uChar.UnicodeChar;
        if (wc == 0x7F)  return Key{KeyType::Backspace, '\0'};
        if (wc == 0x03)  return Key{KeyType::CtrlC, '\0'};
        if (wc == L'\r' || wc == L'\n') return Key{KeyType::Enter, '\0'};
        if (wc < 0x80)   return Key{KeyType::Char, static_cast<char>(wc)};

        // Non-ASCII: convert the single UTF-16 unit to UTF-8. Surrogate pairs
        // arrive as two records; we emit the pieces and the sequence reassembles
        // in the std::string that the query line is built from.
        char buf[4];
        int n = 0;
        DWORD cp = static_cast<DWORD>(wc);
        if (cp < 0x800) {
            buf[n++] = static_cast<char>(0xC0 | (cp >> 6));
            buf[n++] = static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            buf[n++] = static_cast<char>(0xE0 | (cp >> 12));
            buf[n++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            buf[n++] = static_cast<char>(0x80 | (cp & 0x3F));
        }
        return Key{KeyType::Char, buf[0]};
    }
}

int Terminal::getWidth() const {
    CONSOLE_SCREEN_BUFFER_INFO csbi{};
    HANDLE h = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (h != INVALID_HANDLE_VALUE && ::GetConsoleScreenBufferInfo(h, &csbi))
        return csbi.srWindow.Right - csbi.srWindow.Left + 1;
    return 80;
}

int Terminal::getHeight() const {
    CONSOLE_SCREEN_BUFFER_INFO csbi{};
    HANDLE h = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (h != INVALID_HANDLE_VALUE && ::GetConsoleScreenBufferInfo(h, &csbi))
        return csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    return 24;
}

void Terminal::clear() {
    if (!Color::on()) return;
    std::fputs("\x1b[2J\x1b[H", stdout);
    std::fflush(stdout);
}

#endif  // _WIN32

// Detect UTF-8 capability once, from the environment.
static void detectUtf8() {
#ifdef _WIN32
    // Windows: we forced the code page to UTF-8 above, so assume yes.
    g_utf8 = true;
#else
    const char* vars[] = {"LC_ALL", "LC_CTYPE", "LANG"};
    for (const char* v : vars) {
        const char* val = std::getenv(v);
        if (!val || !*val) continue;
        const std::string s(val);
        if (s.find("UTF-8") != std::string::npos ||
            s.find("utf8")  != std::string::npos ||
            s.find("UTF8")  != std::string::npos) { g_utf8 = true; return; }
    }
    g_utf8 = false;
#endif
}

// ---------------------------------------------------------------------------
// 3. Text utilities
//
// The rule everywhere below: never index into a UTF-8 string by byte position
// and never cut one in the middle. A byte-indexed slice would split a
// multi-byte character and emit invalid UTF-8, which the terminal renders as
// replacement garbage.
// ---------------------------------------------------------------------------

// Decodes one code point starting at `i`, advancing `i` past it. Returns 0xFFFD
// (the replacement character) for malformed input and still advances by one,
// so a corrupt byte cannot cause an infinite loop.
static unsigned utf8Decode(const std::string& s, std::size_t& i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    std::size_t extra = 0;
    unsigned cp = 0;

    if (c < 0x80)          { ++i; return c; }
    else if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; }
    else { ++i; return 0xFFFD; }

    // Truncated sequence at the end of the string: report one replacement
    // char and advance past the lead byte so the loop cannot stall.
    if (i + extra >= s.size()) { ++i; return 0xFFFD; }

    for (std::size_t k = 1; k <= extra; ++k) {
        const unsigned char cc = static_cast<unsigned char>(s[i + k]);
        if ((cc & 0xC0) != 0x80) { ++i; return 0xFFFD; }
        cp = (cp << 6) | (cc & 0x3F);
    }
    i += extra + 1;
    return cp;
}

// How many terminal columns one code point occupies. Full Unicode width
// tables are overkill here; these ranges cover CJK, emoji and combining marks,
// which is what actually shows up in filenames.
static int cpWidth(unsigned cp) {
    if (cp == 0) return 0;
    if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) return 0;   // control
    // Combining marks: zero width.
    if ((cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x200B && cp <= 0x200F) ||
        (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0x20D0 && cp <= 0x20FF))
        return 0;
    // Wide ranges: CJK, Hangul, Kana, fullwidth forms, emoji.
    if ((cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0x303E) ||
        (cp >= 0x3041 && cp <= 0x33FF) || (cp >= 0x3400 && cp <= 0x4DBF) ||
        (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xA000 && cp <= 0xA4CF) ||
        (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
        (cp >= 0xFE30 && cp <= 0xFE6F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
        (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1F9FF))
        return 2;
    return 1;
}

// Display width of a whole UTF-8 string.
static int displayWidth(const std::string& s) {
    int w = 0;
    for (std::size_t i = 0; i < s.size();) w += cpWidth(utf8Decode(s, i));
    return w;
}

// Truncate to at most `cols` display columns, appending "..." when it had to
// cut. Walks code point by code point so a multi-byte character is never split.
static std::string truncateToWidth(const std::string& s, int cols) {
    if (cols <= 0) return "";
    if (displayWidth(s) <= cols) return s;
    // Budget smaller than the ellipsis itself: a full "..." would OVERFLOW the
    // column and push the rest of the table out of alignment. Emit exactly as
    // many dots as fit.
    if (cols <= 3) return std::string(static_cast<std::size_t>(cols), '.');

    std::string out;
    int w = 0;
    for (std::size_t i = 0; i < s.size();) {
        const std::size_t start = i;
        const unsigned cp = utf8Decode(s, i);
        const int cw = cpWidth(cp);
        if (w + cw > cols - 3) break;   // leave room for the ellipsis
        out.append(s, start, i - start);
        w += cw;
    }
    return out + "...";
}

// ASCII-only case fold. Deliberately NOT a Unicode tolower: std::tolower on a
// char outside ASCII is locale-dependent, and there is no portable wide-char
// version that works the same on Windows and POSIX.
//
// Why ASCII-only is SAFE, not a compromise: for bytes >= 0x80 this function
// copies the byte through untouched. That means the folded string has exactly
// the same length and the same byte offsets as the original, so the position of
// a match in the folded string is the position of that match in the original.
// That is what lets the renderer highlight the matched substring inside the
// ORIGINAL filename without re-encoding anything — and no multi-byte sequence
// is ever altered, so UTF-8 filenames survive intact.
static std::string asciiFold(const std::string& s) {
    std::string out = s;
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

// 1234567 -> "1.2 MB". Binary units, because that is what file managers report.
static std::string humanSize(std::size_t bytes) {
    static const char* units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 5) { v /= 1024.0; ++u; }
    char buf[32];
    if (u == 0)      std::snprintf(buf, sizeof buf, "%d B", static_cast<int>(v));
    else if (v < 10) std::snprintf(buf, sizeof buf, "%.1f %s", v, units[u]);
    else             std::snprintf(buf, sizeof buf, "%.0f %s", v, units[u]);
    return buf;
}

// Path -> UTF-8 std::string on both platforms.
// In C++20 path::u8string() returns std::u8string (char8_t), in C++17 it
// returns std::string. This helper hides that difference so the rest of the
// program can just deal in std::string.
static std::string pathToUtf8(const fs::path& p) {
#if defined(__cpp_char8_t)
    const std::u8string u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
#else
    return p.u8string();
#endif
}

// ---------------------------------------------------------------------------
// 4. FileNode — one node of the linked list, one entry's metadata.
//
// The constructor auto-fills every field from a path ("zero manual data
// entry"): the caller hands over a path, the constructor does the rest.
// All fields are private (encapsulation); outside code can only read them.
// ---------------------------------------------------------------------------
class FileNode {
private:
    std::string name;        // filename with extension, e.g. "report.pdf"
    std::string extension;   // extension without dot, e.g. "pdf"; "" if none
    std::string fullPath;    // path as discovered
    std::string parentDir;   // directory containing it
    std::size_t size;        // bytes (0 for directories)
    bool isDir;              // directories are indexed too

    FileNode* next;          // singly linked list link — only FileList touches it

public:
    // Primary ctor: auto-fill from a real filesystem path. Takes the path as
    // UTF-8 std::string; u8string() is the only conversion used, so non-ASCII
    // names survive on both platforms.
    explicit FileNode(const std::string& path)
        : fullPath(path), size(0), isDir(false), next(nullptr) {
        const fs::path p(path);

        name = pathToUtf8(p.filename());
        parentDir = pathToUtf8(p.parent_path());

        std::string ext = pathToUtf8(p.extension());
        if (!ext.empty() && ext.front() == '.') ext.erase(0, 1);
        extension = ext;

        // Error-code overloads throughout: the throwing forms throw
        // filesystem_error, and a scan must survive one bad entry.
        std::error_code ec;
        isDir = fs::is_directory(p, ec);
        if (!ec && !isDir) {
            const auto sz = fs::file_size(p, ec);
            if (!ec) size = static_cast<std::size_t>(sz);
        }
    }

    // Secondary ctor: explicit fields — lets tests build nodes without real
    // files on disk.
    FileNode(const std::string& name, const std::string& extension,
             const std::string& fullPath, std::size_t size, bool isDir = false)
        : name(name), extension(extension), fullPath(fullPath),
          parentDir(fs::path(fullPath).parent_path().string()),
          size(size), isDir(isDir), next(nullptr) {}

    const std::string& getName()       const { return name; }
    const std::string& getExtension()  const { return extension; }
    const std::string& getFullPath()   const { return fullPath; }
    const std::string& getParentDir()  const { return parentDir; }
    std::size_t        getSize()       const { return size; }
    bool               isDirectory()   const { return isDir; }
    FileNode*          getNext()       const { return next; }

    // Pre-folded copies of name and extension. Folding is done ONCE at
    // construction instead of on every keystroke: that is the difference
    // between searching-as-you-type being instant and it being a lag.
    // Not const because the fold is lazy — computed on first use, then cached.
    const std::string& getNameFolded() const {
        if (!nameFoldedInit) { nameFolded = asciiFold(name); nameFoldedInit = true; }
        return nameFolded;
    }
    const std::string& getExtFolded() const {
        if (!extFoldedInit) { extFolded = asciiFold(extension); extFoldedInit = true; }
        return extFolded;
    }

    // `next` stays private with FileList as friend: no public setter means
    // no outside code can splice the chain; friendship grants exactly one
    // class (the list) the right to rewire links.
    friend class FileList;

private:
    mutable std::string nameFolded, extFolded;
    mutable bool nameFoldedInit = false, extFoldedInit = false;
};

// ---------------------------------------------------------------------------
// 5. FileList — the in-memory index. A singly linked list that OWNS every node
// (composition). One list == one directory scan; searches read it, never
// mutate it.
//
// Also owns a flat std::vector<const FileNode*> mirror. The linked list is
// the data structure the project is about, but a search that has to walk
// `next` pointers for 200k nodes is pointer-chasing; the vector lets the same
// nodes be scanned linearly, which is what makes the UI responsive. Both
// point at the same nodes, so there is no second copy of the data.
// ---------------------------------------------------------------------------
class FileList {
private:
    FileNode* head;   // first node, nullptr when empty
    FileNode* tail;   // last node, cached so append() is O(1) — no end-walk
    int count;        // maintained on append, read via getCount()
    std::vector<const FileNode*> flat;   // read-only mirror, same nodes

public:
    FileList() : head(nullptr), tail(nullptr), count(0) {}

    // Rule of three: because we manage raw memory, we must say what copying
    // does. Copying is DELETED rather than trusted to us: a shallow copy would
    // leave two lists pointing at the same nodes, and both destructors would
    // free them = double free. Turning that runtime crash into a compile
    // error is the whole point.
    FileList(const FileList&) = delete;
    FileList& operator=(const FileList&) = delete;

    // Frees every node: the list is the single owner, so cleanup lives here
    // and nowhere else.
    ~FileList() {
        FileNode* cur = head;
        while (cur != nullptr) {
            FileNode* dead = cur;
            cur = cur->getNext();
            delete dead;
        }
        head = tail = nullptr;
        count = 0;
    }

    // O(1) append: tail is cached, so we never walk the list.
    FileNode* append(const std::string& fullPath) {
        FileNode* node = new FileNode(fullPath);
        node->next = nullptr;
        if (tail == nullptr) head = tail = node;   // empty list
        else { tail->next = node; tail = node; }
        ++count;
        flat.push_back(node);
        return node;
    }

    // Test append: node built from explicit fields.
    FileNode* append(const std::string& name, const std::string& extension,
                     const std::string& fullPath, std::size_t size,
                     bool isDir = false) {
        FileNode* node = new FileNode(name, extension, fullPath, size, isDir);
        node->next = nullptr;
        if (tail == nullptr) head = tail = node;
        else { tail->next = node; tail = node; }
        ++count;
        flat.push_back(node);
        return node;
    }

    // Visitor traversal of the LIST: strategies see every node READ-ONLY.
    // Nobody gets ownership of the chain, so nobody can break it.
    template <typename Visitor>
    void traverse(Visitor&& visit) const {
        for (FileNode* cur = head; cur != nullptr; cur = cur->getNext())
            visit(cur);
    }

    // Same idea over the flat mirror — cache-seekable, so use it for anything
    // on the hot path.
    template <typename Visitor>
    void traverseFlat(Visitor&& visit) const {
        for (const FileNode* n : flat) visit(n);
    }

    int getCount() const { return count; }
    FileNode* getHead() const { return head; }
    FileNode* getTail() const { return tail; }
    const std::vector<const FileNode*>& getFlat() const { return flat; }

    // Builds a list from nodes we do NOT own — used by the cache loader, which
    // creates FileNodes on the heap and hands over pointers. Ownership is the
    // same either way (FileList deletes them), this just avoids re-running the
    // filesystem probe for a cached entry.
    void adopt(FileNode* node) {
        node->next = nullptr;
        if (tail == nullptr) head = tail = node;
        else { tail->next = node; tail = node; }
        ++count;
        flat.push_back(node);
    }
};

struct Hit {
    const FileNode* node;
    std::size_t matchPos;   // byte offset of the match inside the NAME
    std::size_t matchLen;
};

class ResultSink {
public:
    void add(const FileNode* n, std::size_t pos, std::size_t len) {
        hits.push_back(Hit{n, pos, len});
    }
    void reserve(std::size_t n) { hits.reserve(n); }
    void clear() { hits.clear(); }
    std::vector<Hit>& get() { return hits; }
    const std::vector<Hit>& get() const { return hits; }
    std::size_t size() const { return hits.size(); }
private:
    std::vector<Hit> hits;
};

// ---------------------------------------------------------------------------
// 6. SearchStrategy — the abstract interface. One pure-virtual rule: "given this
// index, produce your results". Pure virtual = cannot instantiate "a search"
// in the abstract, only concrete modes. The index arrives as a parameter, so
// strategies hold no reference to any particular list — that's what makes
// them swappable from the query bar through one base pointer.
// ---------------------------------------------------------------------------
class SearchStrategy {
public:
    virtual ~SearchStrategy() = default;   // virtual: safe delete via base ptr

    // The working set is passed IN, not held by the strategy. That is what
    // makes narrowing possible: the query hands term N only what term N-1
    // kept, so cost is O(survivors) per term instead of O(index) per term.
    // A strategy still holds no reference to any particular FileList, so the
    // three of them stay interchangeable.
    virtual void search(const std::vector<const FileNode*>& candidates,
                        ResultSink& sink) = 0;

    // Convenience overload: search the whole index. Used by tests and by the
    // plain "one term" path.
    void search(const FileList* src, ResultSink& sink) {
        if (src == nullptr) return;
        search(src->getFlat(), sink);
    }
};

// --- Shared result sink. Every strategy writes into the same struct so the
// --- Renderer never needs to know which strategy ran.
// ============================================================================
// SearchByName — exact match or substring, with a case-insensitive toggle.
// ASCII-folded comparison (see asciiFold for why that is UTF-8 safe).
// ============================================================================
class SearchByName : public SearchStrategy {
private:
    std::string query;
    std::string needle;      // pre-folded once, not per node
    bool exactMatch;

public:
    SearchByName(const std::string& query, bool exactMatch = false)
        : query(query), needle(asciiFold(query)), exactMatch(exactMatch) {}

    const std::string& getQuery() const { return query; }

    void search(const std::vector<const FileNode*>& cands,
                ResultSink& sink) override {
        if (needle.empty()) return;

        for (const FileNode* node : cands) {
            const std::string& hay = node->getNameFolded();
            if (exactMatch) {
                if (hay == needle) sink.add(node, 0, hay.size());
            } else {
                const std::size_t pos = hay.find(needle);
                if (pos != std::string::npos) sink.add(node, pos, needle.size());
            }
        }
    }
};

// ============================================================================
// SearchByType — filter by extension. Directories are excluded unless asked
// for: "find me a .cpp" should not return 400 folders.
// ============================================================================
class SearchByType : public SearchStrategy {
private:
    std::string oneExtension;   // "" = match all
    std::string needle;
    bool includeDirs;

public:
    explicit SearchByType(const std::string& extension = "",
                          bool includeDirs = false)
        : oneExtension(asciiFold(extension)), needle(asciiFold(extension)),
          includeDirs(includeDirs) {}

    void search(const std::vector<const FileNode*>& cands,
                ResultSink& sink) override {
        for (const FileNode* node : cands) {
            if (node->isDirectory() && !includeDirs) continue;
            if (needle.empty()) { sink.add(node, 0, 0); continue; }
            if (node->getExtFolded() == needle) sink.add(node, 0, 0);
        }
    }
};

// ============================================================================
// SearchByDirectory — keeps entries whose fullPath starts with a directory
// path. recursive=false: immediate children only. recursive=true: full
// descendants. The prefix must land on a PATH BOUNDARY: "/a/project" must not
// match "/a/project-link" (the "rest" must start with a separator).
// Comparison is byte-exact — POSIX paths are case-sensitive (deliberate).
// ============================================================================
class SearchByDirectory : public SearchStrategy {
private:
    std::string dirPath;
    std::string prefix;        // normalized, pre-computed
    bool recursive;

public:
    SearchByDirectory(const std::string& dirPath, bool recursive = false)
        : dirPath(dirPath), recursive(recursive) {
        prefix = fs::path(dirPath).generic_string();
        while (prefix.size() > 1 && prefix.back() == '/') prefix.pop_back();
        if (prefix == ".") prefix = "";
    }

    void search(const std::vector<const FileNode*>& cands,
                ResultSink& sink) override {
        if (prefix.empty()) return;

        for (const FileNode* node : cands) {
            const std::string& fp = node->getFullPath();
            // `continue`, NOT `return`: this skips ONE entry that does not
            // match. Returning here would abandon every remaining entry, so
            // the first non-matching node would silently empty the results.
            if (fp.size() <= prefix.size()) continue;
            if (fp.compare(0, prefix.size(), prefix) != 0) continue;

            const std::string rest = fp.substr(prefix.size());
            if (rest.empty() || rest[0] != '/') continue;   // boundary violated

            if (!recursive && rest.substr(1).find('/') != std::string::npos)
                continue;                                  // deeper than a child

            sink.add(node, 0, 0);
        }
    }
};

// ---------------------------------------------------------------------------
// 7. Indexer — turns a directory tree into a FileList, ON A BACKGROUND THREAD.
//
// Why a thread: the UI must stay responsive while a scan of C:\ or $HOME runs.
// A scan that blocks the main thread freezes the spinner and makes the program
// look crashed. So the walk runs on a std::thread and communicates progress
// through an atomic counter; the main loop keeps drawing frames while it runs.
//
// Why per-directory iteration, not one recursive iterator: recursive_directory_iterator
// throws when it cannot descend, and recovering from that mid-walk is messy.
// One directory_iterator per directory means a failure costs us that one
// directory's remaining entries, never the whole scan.
//
// Symlinks: never followed (symlink_option::none). A symlink pointing at an
// ancestor is an infinite loop; the OS gives no portable "I already saw this".
// We therefore simply do not descend into symlinked directories.
// ---------------------------------------------------------------------------
class ScanProgress {
public:
    // atomics because the writer is the scan thread and the reader is the UI
    // thread. A plain int would be a data race — undefined behaviour, and in
    // practice a torn or stale count.
    std::atomic<long long> files{0};
    std::atomic<bool>     done{false};
    std::atomic<bool>     failed{false};
    std::atomic<bool>     cancel{false};
    std::mutex            msgMutex;
    std::string           message;
    std::string           root;

    void setMessage(const std::string& m) {
        std::lock_guard<std::mutex> lk(msgMutex);
        message = m;
    }
    std::string getMessage() const {
        std::lock_guard<std::mutex> lk(const_cast<std::mutex&>(msgMutex));
        return message;
    }
    long long getFiles() const { return files.load(); }
    bool isDone() const { return done.load(); }

    // std::atomic is not copy- or move-assignable, so a plain `prog = ScanProgress()`
    // cannot compile. An explicit reset is both clearer at the call site and
    // the only correct way to reuse the object for a second scan.
    void reset() {
        files = 0;
        done = false;
        failed = false;
        cancel = false;
        std::lock_guard<std::mutex> lk(msgMutex);
        message.clear();
    }
};

// Pseudo-filesystems on Linux/macOS: walking these is slow, enormous, and
// generates endless unreadable entries. Skipping them is not an optimization,
// it is what makes a home-directory scan finish.
static bool isPseudoDir(const fs::path& p) {
#ifdef _WIN32
    (void)p;
    return false;
#else
    static const char* pseudo[] = {"/proc", "/sys", "/dev", "/run"};
    const std::string s = p.generic_string();
    for (const char* q : pseudo) {
        const std::size_t n = std::strlen(q);
        if (s.size() >= n && s.compare(0, n, q) == 0 &&
            (s.size() == n || s[n] == '/'))
            return true;
    }
    return false;
#endif
}

// Counts up to `budget` entries, then reports how far it got and where it
// stopped. Capping is what lets the UI show a spinner and a live count
// instead of freezing for 40 seconds on a cold C:\ scan.
class Indexer {
public:
    // Depth-first, one directory_iterator per directory. Every fallible call
    // takes an error_code: we never want an exception escaping a worker thread,
    // because that would call std::terminate.
    static void walk(const fs::path& dir, FileList& list, ScanProgress& prog,
                     long long budget) {
        if (prog.cancel.load()) return;
        if (isPseudoDir(dir)) return;

        std::error_code ec;
        // skip_permission_denied + no symlink following, in one place.
        fs::directory_iterator it(
            dir, fs::directory_options::skip_permission_denied, ec);
        if (ec) return;   // unreadable directory: give up on THIS one, move on

        for (;;) {
            if (prog.cancel.load() || prog.getFiles() >= budget) return;

            // MUST test against the end iterator before dereferencing. `*it`
            // on a past-the-end iterator is undefined behaviour and reads
            // garbage — in practice a segfault at the end of every directory.
            if (it == fs::directory_iterator()) return;

            const fs::directory_entry& entry = *it;

            std::error_code e2;
            const bool isDir = entry.is_directory(e2);
            if (e2) { ++it; continue; }        // vanished / not a dir

            // Windows: refuse to descend into reparse points (junctions, which
            // include the classic "Documents and Settings" cycle). POSIX:
            // refuse symlinks. Both boil down to "do not follow a link".
#if defined(_WIN32)
            const DWORD attrs = ::GetFileAttributesW(entry.path().c_str());
            const bool isLink = (attrs == INVALID_FILE_ATTRIBUTES) ||
                                (attrs & FILE_ATTRIBUTE_REPARSE_POINT);
#else
            const bool isLink = entry.is_symlink(e2);
#endif
            if (e2) { ++it; continue; }

            const std::string p = pathToUtf8(entry.path());
            list.append(p);
            const long long n = ++prog.files;

            if ((n & 0x3FF) == 0) {           // progress every 1024 entries
                prog.setMessage(p);
                std::this_thread::sleep_for(std::chrono::milliseconds(0));
            }

            if (isDir && !isLink)
                walk(entry.path(), list, prog, budget);

            it.increment(ec);
            if (ec) return;   // could not advance: this directory is finished
        }
    }

    // Entry point for the worker thread.
    static void run(const std::string& rootPath, FileList& list,
                    ScanProgress& prog, long long budget) {
        prog.root = rootPath;
        std::error_code ec;
        const fs::path root(rootPath);

        if (!fs::exists(root, ec) || ec) {
            prog.failed = true;
            prog.setMessage("path does not exist");
            prog.done = true;
            return;
        }
        if (fs::is_regular_file(root, ec) && !ec) {
            list.append(rootPath);
            prog.files = 1;
            prog.done = true;
            return;
        }
        walk(root, list, prog, budget);
        prog.done = true;
    }
};

// ---------------------------------------------------------------------------
// 8. Cache — persist the index so a second run is instant.
//
// Format: a magic + version header, then one TAB-separated line per entry.
// TAB-separated because it is the one ASCII byte that essentially cannot
// appear in a filename on Windows or POSIX (it is legal but rare, and we
// escape it anyway) — so it needs no escaping in 99.999% of real cases.
//
// Text, not binary, on purpose: a corrupt or truncated cache must never crash
// the program. Every line is validated on load and a bad line is skipped.
// ---------------------------------------------------------------------------
class Cache {
private:
    // Where to put the file: APPDATA on Windows, ~/.cache on POSIX. Both are
    // the platform's conventional per-user writable location.
    static fs::path cacheDir() {
#ifdef _WIN32
        if (const char* a = std::getenv("APPDATA")) return fs::path(a) / "QueryFS";
#else
        if (const char* h = std::getenv("HOME"))
            return fs::path(h) / ".cache" / "queryfs";
#endif
        return fs::temp_directory_path() / "queryfs";
    }
    static fs::path cacheFile(const std::string& root) {
        // One cache file per root. The root is hashed so that
        // /home/me and /home/me2 cannot collide, and so the name stays short
        // and filesystem-safe on every platform.
        std::size_t h = 1469598103934665603ULL;   // FNV-1a offset basis
        for (unsigned char c : root) {
            h ^= c;
            h *= 1099511628211ULL;                // FNV prime
        }
        char buf[32];
        std::snprintf(buf, sizeof buf, "%016llx",
                      static_cast<unsigned long long>(h));
        return cacheDir() / (std::string("index-") + buf + ".txt");
    }
    static std::string escape(const std::string& s) {
        std::string o;
        o.reserve(s.size());
        for (char c : s) {
            if (c == '\\')      o += "\\\\";
            else if (c == '\t') o += "\\t";
            else if (c == '\n') o += "\\n";
            else                o += c;
        }
        return o;
    }
    static std::string unescape(const std::string& s) {
        std::string o;
        o.reserve(s.size());
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\\' && i + 1 < s.size()) {
                const char n = s[++i];
                if (n == '\\')      o += '\\';
                else if (n == 't') o += '\t';
                else if (n == 'n') o += '\n';
                else { o += '\\'; o += n; }
            } else o += s[i];
        }
        return o;
    }
    static std::vector<std::string> splitTabs(const std::string& line) {
        std::vector<std::string> out;
        std::size_t start = 0;
        while (true) {
            const std::size_t t = line.find('\t', start);
            if (t == std::string::npos) { out.push_back(line.substr(start)); break; }
            out.push_back(line.substr(start, t - start));
            start = t + 1;
        }
        return out;
    }

public:
    // Returns the number of entries restored, 0 if there was no usable cache.
    static long long load(const std::string& root, FileList& list) {
        std::error_code ec;
        std::ifstream in(cacheFile(root), std::ios::binary);
        if (!in) return 0;

        std::string header;
        if (!std::getline(in, header)) return 0;
        if (header.rfind("QUERYFS-CACHE", 0) != 0) return 0;   // wrong magic

        long long n = 0;
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            const std::vector<std::string> f = splitTabs(line);
            if (f.size() < 4) continue;                 // malformed -> skip
            // A corrupt line can still HAVE four fields: a row of bare tabs
            // splits into four empty strings and would sail past the count
            // check, producing a phantom entry with an empty path that later
            // renders as a blank table row. An entry with no path is not a
            // real entry, so reject it explicitly.
            if (f[2].empty()) continue;
            // A truncated write usually shows up as a short final line; the
            // field-count check above catches it.
            const std::string nm  = unescape(f[0]);
            const std::string ex  = unescape(f[1]);
            const std::string fp  = unescape(f[2]);
            const unsigned long long sz =
                std::strtoull(f[3].c_str(), nullptr, 10);
            const bool isDir = (f.size() > 4 && f[4] == "1");
            list.adopt(new FileNode(nm, ex, fp,
                                    static_cast<std::size_t>(sz), isDir));
            ++n;
        }
        return n;
    }

    static bool save(const std::string& root, const FileList& list) {
        std::error_code ec;
        fs::create_directories(cacheDir(), ec);
        if (ec) return false;

        const fs::path tmp = cacheFile(root);
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;

        out << "QUERYFS-CACHE\t2\n";
        list.traverseFlat([&](const FileNode* n) {
            out << escape(n->getName()) << '\t'
                << escape(n->getExtension()) << '\t'
                << escape(n->getFullPath()) << '\t'
                << n->getSize() << '\t'
                << (n->isDirectory() ? '1' : '0') << '\n';
        });
        out.flush();
        const bool ok = out.good();
        out.close();
        return ok;
    }

    // Drops the cache for a root — called before a background RESCAN so we
    // never write a partial index over a good one.
    static void invalidate(const std::string& root) {
        std::error_code ec;
        fs::remove(cacheFile(root), ec);
    }
};

// ---------------------------------------------------------------------------
// 9. Query — the query language.
//
//   /name <text>      substring in filename            (default if no prefix)
//   /ext  <text>      extension, with or without dot
//   /dir  <path>      entries under a path
//   /dironly          restrict results to directories
//   /fileonly         restrict results to files
//   /r                (with /dir) full descendants instead of immediate
//
// Terms are separated by spaces and ANDed together, so
//     /ext cpp /name util
// means "a .cpp file whose name contains util" — and because each term
// narrows the working set before the next one runs, the second term is only
// tested against what the first one kept.
//
// Three terms are handled specially and never become a SearchStrategy:
// /dironly, /fileonly and /r are FLAGS. Making them strategies would be
// modelling something that is not a search.
// ---------------------------------------------------------------------------
struct QueryTerm {
    enum Kind { Name, Ext, Dir } kind;
    std::string value;
    bool recursive = false;
};

class Query {
private:
    std::vector<QueryTerm> terms;
    bool dirOnly = false;
    bool fileOnly = false;

public:
    // Returns false when the query cannot mean anything (e.g. empty text after
    // a prefix) so the caller can keep the previous results on screen.
    bool parse(const std::string& line) {
        terms.clear();
        dirOnly = fileOnly = false;

        // Tokenize on spaces, but keep quoted sections together so
        // /dir "my folder/x" works for paths containing spaces.
        std::vector<std::string> tokens;
        std::string cur;
        bool inQuote = false;
        for (char c : line) {
            if (c == '"') { inQuote = !inQuote; continue; }
            if (c == ' ' && !inQuote) {
                if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
                continue;
            }
            cur += c;
        }
        if (!cur.empty()) tokens.push_back(cur);

        for (std::size_t i = 0; i < tokens.size(); ++i) {
            const std::string& t = tokens[i];
            if (t.empty()) continue;

            if (t[0] != '/') {   // bare word: an implicit /name term
                terms.push_back({QueryTerm::Name, t, false});
                continue;
            }

            const std::string cmd = asciiFold(t.substr(1));
            auto value = [&](std::string& out) -> bool {
                if (i + 1 < tokens.size()) { out = tokens[++i]; return !out.empty(); }
                return false;
            };

            std::string v;
            if (cmd == "name" || cmd == "n") {
                if (!value(v)) return false;
                terms.push_back({QueryTerm::Name, v, false});
            } else if (cmd == "ext" || cmd == "e") {
                if (!value(v)) return false;
                // Accept ".cpp" and "cpp" alike — the dot is a detail of the
                // user's typing, not part of the data we store.
                if (!v.empty() && v.front() == '.') v.erase(0, 1);
                terms.push_back({QueryTerm::Ext, v, false});
            } else if (cmd == "dir" || cmd == "d") {
                if (!value(v)) return false;
                bool rec = false;
                if (i + 1 < tokens.size() &&
                    (tokens[i+1] == "/r" || asciiFold(tokens[i+1]) == "/r")) {
                    rec = true; ++i;
                }
                terms.push_back({QueryTerm::Dir, v, rec});
            } else if (cmd == "dironly") {
                dirOnly = true;
            } else if (cmd == "fileonly") {
                fileOnly = true;
            } else if (cmd == "help" || cmd == "h" || cmd == "?") {
                terms.push_back({QueryTerm::Name, "/help", false});
            } else {
                return false;   // unknown command: reject the whole query
            }
        }
        return true;
    }

    bool empty() const { return terms.empty(); }
    std::size_t size() const { return terms.size(); }
    const std::vector<QueryTerm>& getTerms() const { return terms; }
    bool wantDirs() const { return dirOnly; }
    bool wantFiles() const { return fileOnly; }

    // Runs the terms in order, each over only the previous term's output.
    // Returns the number of surviving hits.
    //
    // The narrowing is the point: with an index of 200k entries, "/ext cpp
    // /name util" tests the substring against the few thousand .cpp files
    // instead of all 200k. Each term is O(remaining) rather than O(total) x
    // terms, and the strategy objects are still dispatched through the
    // SearchStrategy base pointer, so the polymorphic design is preserved
    // rather than replaced by a hand-rolled conjunction.
    std::size_t run(const FileList* index, ResultSink& out) const {
        out.clear();
        if (index == nullptr) return 0;

        std::vector<const FileNode*> candidates(index->getFlat().begin(),
                                                index->getFlat().end());
        // Reserve once for the output; without this the vector reallocates
        // logarithmically while we append, which for a 300k-hit query means
        // thousands of copies.
        out.reserve(candidates.size());

        // A flag term is applied as a filter pass, not a strategy.
        auto applyFlag = [&](bool wantDir) {
            std::vector<const FileNode*> keep;
            keep.reserve(candidates.size());
            for (const FileNode* n : candidates)
                if (n->isDirectory() == wantDir) keep.push_back(n);
            candidates.swap(keep);
        };

        for (const QueryTerm& t : terms) {
            if (t.value == "/help") continue;
            if (candidates.empty()) break;

            ResultSink stage;
            std::unique_ptr<SearchStrategy> strategy;
            switch (t.kind) {
                case QueryTerm::Name:
                    strategy = std::make_unique<SearchByName>(t.value, false);
                    break;
                case QueryTerm::Ext:
                    strategy = std::make_unique<SearchByType>(t.value, false);
                    break;
                case QueryTerm::Dir:
                    strategy = std::make_unique<SearchByDirectory>(t.value,
                                                                  t.recursive);
                    break;
            }
            if (!strategy) continue;

            // Hand the strategy ONLY the survivors so far. This is the
            // narrowing: no intersection pass, no O(n*m) membership test.
            strategy->search(candidates, stage);
            candidates.clear();
            candidates.reserve(stage.size());
            for (const Hit& h : stage.get()) candidates.push_back(h.node);
        }

        if (dirOnly) applyFlag(true);
        else if (fileOnly) applyFlag(false);

        // Fold the NAME needles ONCE, here, not once per surviving row. Doing
        // it inside the row loop re-ran asciiFold over the same short string
        // tens of thousands of times per keystroke on a large index, which is
        // pure waste: the needles do not change while we iterate rows.
        std::vector<std::string> nameNeedles;
        nameNeedles.reserve(terms.size());
        for (const QueryTerm& t : terms) {
            if (t.kind != QueryTerm::Name || t.value == "/help") continue;
            const std::string folded = asciiFold(t.value);
            if (!folded.empty()) nameNeedles.push_back(folded);
        }

        for (const FileNode* n : candidates) {
            // Highlight offsets for the NAME terms only — the name is the one
            // column that highlights.
            std::size_t pos = 0, len = 0;
            const std::string& hay = n->getNameFolded();
            for (const std::string& ndl : nameNeedles) {
                const std::size_t p = hay.find(ndl);
                if (p != std::string::npos) { pos = p; len = ndl.size(); break; }
            }
            out.add(n, pos, len);
        }
        return out.size();
    }
};

// ---------------------------------------------------------------------------
// 10. Theme — colours, box-drawing glyphs and the extension colour table.
//
// One place decides how things look, so --no-color / NO_COLOR is honoured
// everywhere at once and the Renderer never asks "should I colour this?".
// ---------------------------------------------------------------------------
namespace Theme {
// 256-colour codes: a cyan -> magenta ramp for the banner, distinct hues for
// the file categories below.
static const int BANNER_A = 45;   // cyan
static const int BANNER_B = 201;  // magenta
static const int PROMPT   = 51;
static const int HEADER   = 245;
static const int SELBG    = 236;
static const int STATUS   = 244;
static const int WARN     = 208;
static const int HILITE   = 15;

enum ExtCat { CODE, IMAGE, DOC, ARCHIVE, OTHER };
inline ExtCat categorise(const std::string& ext) {
    static const char* code[] = {"cpp","c","h","hpp","py","js","ts","java","go","rs",
                                 "php","rb","swift","kt","cs","sh","sql","lua","pl",
                                 "json","yaml","yml","toml","xml","html","css","scss"};
    static const char* image[]= {"png","jpg","jpeg","gif","bmp","webp","svg","ico",
                                 "tiff","tif","heic","avif","psd"};
    static const char* doc[]  = {"pdf","doc","docx","txt","md","rtf","odt","tex",
                                 "epub","ppt","pptx","xls","xlsx","csv","log"};
    static const char* arch[] = {"zip","tar","gz","bz2","xz","7z","rar","iso",
                                 "dmg","tgz","jar","whl","deb","rpm"};
    const std::string e = asciiFold(ext);
    for (const char* c : code)  if (e == c) return CODE;
    for (const char* c : image) if (e == c) return IMAGE;
    for (const char* c : doc)   if (e == c) return DOC;
    for (const char* c : arch)  if (e == c) return ARCHIVE;
    return OTHER;
}
inline int extColour(const std::string& ext) {
    switch (categorise(ext)) {
        case CODE:   return 81;   // light blue
        case IMAGE:  return 205;  // pink
        case DOC:    return 220;  // amber
        case ARCHIVE:return 114;  // green
        default:     return 250;  // grey
    }
}

// Box-drawing when the terminal is UTF-8, plain ASCII otherwise. Selected by
// the same flag that gates colour, because a terminal that cannot do 256
// colours usually cannot do box-drawing either.
struct Glyphs {
    const char* h;    // horizontal
    const char* v;    // vertical
    const char* tl, *tr, *bl, *br;
    const char* bar;  // spinner frames
};
inline Glyphs glyphs(bool fancy) {
    return fancy ? Glyphs{"\u2500","\u2502","\u250c","\u2510","\u2514","\u2518",
                          "|/-\\"}
                 : Glyphs{"-","|","+","+","+","+",".oO@*"};
}
} // namespace Theme

// ---------------------------------------------------------------------------
// 11. Renderer — builds the ENTIRE frame as one std::string, then writes it
// once.
//
// Why one buffer: search-as-you-type repaints on every keystroke. If drawing
// issued one write() per cell or per colour change, the terminal would spend
// its time in the write syscall and the UI would visibly stutter. Assembling
// the frame in memory and emitting it with a single fwrite makes the cost
// proportional to the frame size, not to the number of styling operations.
//
// \x1b[H homes the cursor instead of clearing: no flash, and the previous
// frame is overwritten in place. Lines are padded to the full width and each
// ends with \x1b[K so leftovers from a longer previous line cannot linger.
// ---------------------------------------------------------------------------
class Renderer {
private:
    const Terminal& term;
    std::string frame;
    int width = 80;

    // Reusable buffers: the Renderer is called ~60x/second and must not
    // allocate a fresh frame string each time if we can help it.
    std::string statusCache;

    void put(const std::string& s) { frame += s; }
    void padTo(int targetCols) {
        // Pad using display width, not byte length, or a UTF-8 row would be
        // over-padded and wrap.
        int w = 0;
        for (std::size_t i = 0; i < frame.size();) w += cpWidth(utf8Decode(frame, i));
        if (w < targetCols) frame.append(static_cast<std::size_t>(targetCols - w), ' ');
    }

public:
    // Original block-letter wordmark, hand-built. Each letter is 6 rows of 5
    // columns; stored as a raw string literal so the backslashes in the
    // gradient helper need no escaping.
    static const char* banner() {
        return
"  ____  ____  ____  __     _   _ ____  \n"
" / ___||  _ \\|  _ \\|  |   | | | |  _ \\ \n"
"| |    | | | | |_) |  |   | |_| | | | |\n"
"| |___ | |_| |  _ <|  |___ |  _  | |_| |\n"
" \\____||____/|_| \\_\\____|___||_| |____/ \n"
"\n"
"      index once \u00b7 query forever\n"
"      Q U E R Y F S   v2.0   \u2014 C++17, zero dependencies\n";
    }

    explicit Renderer(const Terminal& t) : term(t) {}

    void setWidth(int w) { width = w < 20 ? 20 : w; }

    // The banner, printed once at startup with a cyan->magenta gradient
    // cycling down the lines.
    static void printBanner() {
        if (!Color::on()) { std::fputs(banner(), stdout); return; }
        // Split the logo into lines so each can take the next ramp colour.
        const std::string all(banner());
        std::size_t pos = 0;
        int step = 0;
        while (pos < all.size()) {
            std::size_t nl = all.find('\n', pos);
            if (nl == std::string::npos) nl = all.size();
            const std::string line = all.substr(pos, nl - pos + 1);
            // Interpolate between cyan and magenta by line index.
            const int totalLines = 11;
            const int t2 = (totalLines > 1) ? (step * (Theme::BANNER_B - Theme::BANNER_A))
                                           / (totalLines - 1) : 0;
            std::string painted = Color::paint(Theme::BANNER_A + t2, line);
            std::fputs(painted.c_str(), stdout);
            ++step;
            pos = nl + 1;
        }
        std::fputs(Color::reset().c_str(), stdout);
        std::fflush(stdout);
    }

    // One full frame. `rows` is the number of result rows to draw.
    void draw(const std::string& query, const ResultSink& results,
              std::size_t selected, const std::string& statusLeft,
              int resultCount, double searchMs) {
        const int h = term.getHeight();
        const int w = width;
        frame.clear();
        frame.reserve(static_cast<std::size_t>(h) *
                      static_cast<std::size_t>(w + 16));

        // Box-drawing glyphs only when we are actually styling AND the
        // terminal claims UTF-8. Widths are computed on DISPLAY columns so a
        // multi-byte glyph does not break alignment.
        // Box-drawing is tied to STYLING being on, not merely to the terminal
        // being UTF-8-capable: with --no-color / NO_COLOR we drop to plain ASCII
        // glyphs so the output stays readable in a dumb log or a pipe.
        const bool fancy = Color::on() && term.utf8();
        const Theme::Glyphs g = Theme::glyphs(fancy);

        // Home first. Drawing relies on overwriting the previous frame in
        // place, which only works if every frame starts at row 0 — otherwise
        // the cursor stays where the last frame ended and frames smear down
        // the screen.
        if (Color::on()) put("\x1b[H");

        // ---- top: prompt line with a live query and a block cursor
        put(Color::paint(Theme::PROMPT, "queryfs"));
        put(" " + std::string(1, '>') + " ");
        put(query);
        // Block cursor when styling is available; a plain caret otherwise, so
        // NO_COLOR output contains no non-ASCII glyphs at all.
        put(Color::paint(Theme::PROMPT, fancy ? "\u2588" : "_"));
        put("\r\n");

        // ---- separator
        put(Color::paint(Theme::HEADER, std::string(g.h) + std::string(g.h)));
        put("\r\n");

        // ---- results table header
        if (Color::on()) put(Color::paint(Theme::HEADER, "  NAME  EXT   SIZE     PATH"));
        else              put("  NAME  EXT   SIZE     PATH");
        put("\r\n");

        // ---- results
        const int rows = h - 6;   // prompt, separator, header, status, hint, blank
        const int listTop = 0;

        int start = 0;
        if (resultCount > 0) {
            // Keep the selection inside the visible window.
            if (static_cast<int>(selected) < listTop)
                start = static_cast<int>(selected);
            else if (static_cast<int>(selected) >= listTop + rows)
                start = static_cast<int>(selected) - rows + 1;
        }
        if (start < 0) start = 0;

        int drawn = 0;
        for (int i = start;
             i < resultCount && drawn < rows;
             ++i, ++drawn) {
            const Hit& hit = results.get()[static_cast<std::size_t>(i)];
            const FileNode* n = hit.node;

            const bool isSel = (static_cast<std::size_t>(i) == selected);
            const int extCol = 150, sizeCol = 170;

            // Column budget: name gets the slack.
            int nameW = w - extCol - sizeCol - 40;
            if (nameW < 12) nameW = 12;

            if (isSel)
                put(Color::paint(Theme::SELBG, " "));
            else
                put("  ");

            // --- filename, with the matched substring highlighted.
            // Because the fold preserves byte offsets, we can slice the
            // ORIGINAL name at the match position without any re-encoding.
            const std::string& nm = n->getName();
            std::string before, match, after;
            if (hit.matchLen > 0 &&
                hit.matchPos + hit.matchLen <= nm.size()) {
                before = nm.substr(0, hit.matchPos);
                match  = nm.substr(hit.matchPos, hit.matchLen);
                after  = nm.substr(hit.matchPos + hit.matchLen);
            } else {
                before = nm;
            }

            const int wBefore = displayWidth(before);
            const int wMatch  = displayWidth(match);
            const int wAfter  = displayWidth(after);
            int cut = -1;
            if (wBefore + wMatch + wAfter > nameW) {
                // Too wide: decide what to keep. Keep as much of the
                // filename (from the left) as fits, but never hide the match
                // itself if we can help it.
                int room = nameW - 3;
                if (wBefore >= room && wMatch < nameW) {
                    // Match starts beyond the cut -> show the tail so the
                    // user still sees what they typed matching.
                    const int skip = wBefore - (room - wMatch);
                    cut = skip;
                } else if (wBefore + wMatch > room) {
                    cut = room - wMatch;
                }
            }

            std::string shown, pre, mid, post;
            if (cut >= 0) {
                std::size_t bi = 0, mi = 0;
                int acc = 0;
                while (bi < before.size() && acc < cut) {
                    std::size_t st = bi;
                    acc += cpWidth(utf8Decode(before, bi));
                    (void)st;
                }
                // mi = byte offset of `cut` columns into `before`
                mi = bi;
                const int midRoom = nameW - 3 - (cut - (wBefore - displayWidth(std::string(before, 0, mi))));
                pre = std::string("...") + before.substr(mi);
                int acc2 = 0;
                std::size_t ai = 0;
                while (ai < after.size() && acc2 < midRoom) {
                    acc2 += cpWidth(utf8Decode(after, ai));
                }
                mid = match.substr(0, match.size()); (void)mid;
                post = after.substr(0, ai);
                shown = pre + mid + post;
            } else {
                shown = before + match + after;
            }
            // Hard guarantee: never exceed the column, regardless of the
            // maths above.
            shown = truncateToWidth(shown, nameW);
            // Re-split so the highlight survives the truncation.
            pre.clear(); mid.clear(); post.clear();
            if (hit.matchLen > 0 && cut >= 0) {
                const std::size_t mpos = shown.find(match);
                if (mpos != std::string::npos) {
                    pre  = shown.substr(0, mpos);
                    mid  = shown.substr(mpos, match.size());
                    post = shown.substr(mpos + match.size());
                } else { pre = shown; }
            } else {
                pre = shown;
            }

            const int nameColour = n->isDirectory() ? Theme::HEADER
                                                    : Theme::extColour(n->getExtension());
            if (Color::on()) {
                std::string nameCol = Color::paint(nameColour, pre);
                if (!mid.empty())
                    nameCol += Color::paint(Theme::HILITE, mid);
                nameCol += Color::paint(nameColour, post);
                put(nameCol);
            } else {
                put(pre);
                if (!mid.empty()) put("[" + mid + "]");   // readable w/o colour
                put(post);
            }

            padTo(extCol);
            const std::string extTxt = n->isDirectory()
                ? (fancy ? "\u25be" : "/") : n->getExtension();
            if (Color::on()) put(Color::paint(nameColour, extTxt));
            else              put(extTxt);

            padTo(sizeCol);
            const std::string szTxt = n->isDirectory() ? "-" : humanSize(n->getSize());
            if (Color::on()) put(Color::paint(Theme::STATUS, szTxt));
            else              put(szTxt);

            padTo(w - 6);
            std::string path = n->getFullPath();
            // Show the PARENT path, not the full path: the name is already in
            // the first column, so repeating it wastes the width.
            const std::string parent = n->getParentDir();
            path = parent.empty() ? path : parent;
            path = truncateToWidth(path, w - sizeCol - 6);
            if (Color::on()) put(Color::paint(Theme::SELBG, path));
            else              put(path);

            padTo(w);
            if (Color::on()) put(Color::reset());
            put("\x1b[K\r\n");
        }

        // Blank the unused rows so a shorter result set does not leave the
        // previous frame's rows visible underneath.
        for (; drawn < rows; ++drawn) {
            padTo(w);
            put("\x1b[K\r\n");
        }

        // ---- bottom: status bar + key hints
        put(Color::paint(Theme::STATUS, std::string(g.h)));
        put("\r\n");
        put(statusLeft);
        if (Color::on()) put(Color::reset());
        {
            char buf[64];
            std::snprintf(buf, sizeof buf, "   %d match", resultCount);
            std::string t(buf);
            if (resultCount != 1) t += "es";
            std::snprintf(buf, sizeof buf, "   %.2f ms", searchMs);
            t += buf;
            padTo(w - 34);
            put(Color::paint(Theme::STATUS, t));
        }
        padTo(w);
        put("\x1b[K\r\n");
        put(Color::paint(Theme::STATUS, " Esc quit   Up/Down scroll   /help keys"));
        padTo(w);
        put("\x1b[K\r\n");

        // Single write for the whole frame.
        std::fwrite(frame.data(), 1, frame.size(), stdout);
        std::fflush(stdout);
    }
};

// ---------------------------------------------------------------------------
// 12. main
//
// Two modes, chosen at startup:
//   TTY      -> raw-mode interactive UI (arrows, live search)
//   not a TTY-> line-based mode, same query language, plain output.
//
// The fallback exists because the program must not hang when its input is a
// pipe: raw mode on a pipe never delivers a key, so the UI would sit there
// forever. Detecting it and switching is cheaper than trying to make raw mode
// work on a pipe.
// ---------------------------------------------------------------------------

// Where to look when the user gives no path: the home directory on POSIX,
// C:\ on Windows. Reading $HOME is the right call rather than a hardcoded
// path, because it is what the user actually means by "my files".
static std::string defaultRoot() {
#ifdef _WIN32
    if (const char* up = std::getenv("USERPROFILE")) return up;
    return "C:\\";
#else
    if (const char* h = std::getenv("HOME")) return h;
    return "/";
#endif
}

// The status-bar left half: scan state or ready state.
static std::string statusText(const ScanProgress& prog, bool scanning,
                              int spin, bool fancy) {
    if (scanning || !prog.isDone()) {
        static const char* frames[] = {"|", "/", "-", "\\"};
        std::string bar;
        if (fancy) {
            const char* uni[] = {"\u2801","\u2809","\u2839","\u2838"};
            bar = uni[spin % 4];
        } else {
            bar = frames[spin % 4];
        }
        char buf[128];
        std::snprintf(buf, sizeof buf, " %s Indexing %lld files", bar.c_str(),
                      prog.getFiles());
        std::string s(buf);
        // Show the directory we are currently inside, trimmed.
        const std::string where = prog.getMessage();
        if (!where.empty()) {
            std::string tail = where;
            if (displayWidth(tail) > 46) tail = tail.substr(tail.size() - 46);
            s += "  " + tail;
        }
        return s;
    }
    // The tick is a glyph, so it follows the same fancy/plain rule as the
    // box-drawing characters: with --no-color it degrades to ASCII.
    char buf[160];
    std::snprintf(buf, sizeof buf, " %s Index ready: %lld files",
                  fancy ? "\u2714" : "*", prog.getFiles());
    return buf;
}

static void printHelp() {
    std::cout <<
"\nQueryFS keys and query syntax\n"
"  -----------------------------\n"
"  type            live search as you type\n"
"  Esc             quit\n"
"  Up / Down       move the highlighted row\n"
"  PgUp / PgDn     jump a page\n"
"  r               rescan in the background and refresh the cache\n"
"  /               clear the query\n"
"\n"
"  query syntax (terms are ANDed, e.g. /ext cpp /name util)\n"
"    /name <text>   filename contains <text>\n"
"    /ext  <ext>    extension, with or without a leading dot\n"
"    /dir  <path>   entries under <path> (immediate children)\n"
"    /dir <path> /r full descendants instead\n"
"    /dironly       only directories\n"
"    /fileonly      only files\n"
"    \"quotes\"       paths or names containing spaces\n"
"    bareword       same as /name <bareword>\n";
}

static int runInteractive(Terminal& term, std::string root) {
    FileList index;              // the index queries read from
    ScanProgress prog;
    ResultSink results;
    Renderer ui(term);

    term.enterRawMode();
    ui.printBanner();

    // --- Background scan.
    //
    // Thread lifetime is the thing to get right here. A detached thread whose
    // captured state dies with the enclosing function is a use-after-free, so
    // every thread here is JOINED before this function returns. Two cases:
    //
    //   cold start (no cache): the scan fills `index` itself. Queries run
    //   against the partial list while it fills, which is exactly what the
    //   spinner and the live count are for.
    //
    //   warm start (cache hit): `index` is already usable, so the fresh scan
    //   goes into a SEPARATE list owned by a shared_ptr. It is never swapped in
    //   underneath the user mid-typing; instead it becomes the cache for the
    //   NEXT run. No half-built index is ever visible.
    std::shared_ptr<FileList> refresh;
    std::shared_ptr<ScanProgress> refreshProg;
    std::thread scanner;

    const long long warm = Cache::load(root, index);
    if (warm > 0) {
        prog.files = warm;
        prog.done = true;
        refresh     = std::make_shared<FileList>();
        refreshProg = std::make_shared<ScanProgress>();
        scanner = std::thread([&, refresh, refreshProg]{
            Indexer::run(root, *refresh, *refreshProg, 4000000);
        });
    } else {
        scanner = std::thread([&]{ Indexer::run(root, index, prog, 4000000); });
    }

    std::string query;
    std::size_t selected = 0;
    double lastMs = 0;
    int spin = 0;
    std::string lastQuery = "\x01";   // impossible value: forces the first run
    bool showHelp = false;
    bool showHelpPending = false;
    int  lastW = -1, lastH = -1;
    long long scanShown = -1;

    // Rescan requests are deferred to a safe point rather than started from
    // inside the key handler, so a scan is never launched twice in one frame.
    bool wantRescan = false;

    bool running = true;
    bool dirty = true;              // forces the first frame
    while (running) {
        if (wantRescan) {
            wantRescan = false;
            if (scanner.joinable()) scanner.join();
            Cache::invalidate(root);
            prog.reset();
            scanner = std::thread([&]{ Indexer::run(root, index, prog, 4000000); });
            lastQuery = "\x01";       // force a re-query against the fresh index
        }

        const bool scanning = !prog.isDone();
        if (scanning) ++spin;         // animate only while there is work

        const int w = term.getWidth();
        const int h = term.getHeight();

        if (w != lastW || h != lastH) { lastW = w; lastH = h; dirty = true; }

        // Re-query only when the text changed. Re-running an identical query
        // every frame would burn CPU for nothing.
        if (query != lastQuery) {
            auto t0 = std::chrono::steady_clock::now();
            results.clear();
            if (!query.empty()) {
                Query p;
                if (p.parse(query)) p.run(&index, results);
            }
            lastMs = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t0).count();
            lastQuery = query;
            selected = 0;
            dirty = true;
        }
        if (results.size() != 0 && selected >= results.size()) {
            selected = results.size() - 1;
            dirty = true;
        }

        // The live file count changes while scanning, so those frames are
        // dirty too. Once the index is ready and nothing is being typed, the
        // frame is identical — redrawing it would burn CPU for nothing and
        // keep the terminal permanently busy.
        if (scanning) dirty = true;
        if (scanShown != prog.getFiles()) { scanShown = prog.getFiles(); dirty = true; }
        if (showHelpPending != showHelp) { showHelpPending = showHelp; dirty = true; }

        if (dirty) {
            ui.setWidth(w);
            const bool fancy = Color::on() && term.utf8();
            ui.draw(query, results, selected,
                    statusText(prog, scanning, spin, fancy),
                    static_cast<int>(results.size()), lastMs);
            // RESET the flag. Without this it stays true forever and the UI
            // repaints at full speed even when nothing changed — the terminal
            // never goes idle and the CPU is pinned.
            dirty = false;
        }

        if (showHelp) {
            printHelp();
            showHelp = false;
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }

        const Key k = term.readKey();
        // A timeout with no key means "nothing typed"; loop so the spinner
        // keeps animating instead of blocking.
        if (k.type == KeyType::None) {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            continue;
        }

        // Any keypress may change what should be on screen; re-evaluate the
        // frame rather than tracking every case.
        dirty = true;

        switch (k.type) {
            case KeyType::CtrlC:
            case KeyType::Esc:
            case KeyType::Eof:
                running = false;
                break;
            case KeyType::Enter:
                break;                       // queries apply as you type
            case KeyType::Backspace:
                if (!query.empty()) {
                    // Step back to a UTF-8 LEAD byte first, so a multi-byte
                    // character is deleted whole instead of leaving a
                    // fragment that would corrupt the query.
                    std::size_t i = query.size() - 1;
                    while (i > 0 &&
                           (static_cast<unsigned char>(query[i]) & 0xC0) == 0x80)
                        --i;
                    query.erase(i);
                }
                break;
            case KeyType::Up:    if (selected > 0) { --selected; } break;
            case KeyType::Down:  if (selected + 1 < results.size()) { ++selected; } break;
            case KeyType::PageUp:   selected = (selected > 16) ? selected - 16 : 0; break;
            case KeyType::PageDown:
                selected += 16;
                if (results.size() != 0 && selected >= results.size())
                    selected = results.size() - 1;
                break;
            case KeyType::Home: selected = 0; break;
            case KeyType::End:
                if (results.size() != 0) selected = results.size() - 1;
                break;
            case KeyType::Char:
                if (k.ch == 0x0D) { wantRescan = true; break; }   // Enter = rescan
                if (k.ch == 0x12) { wantRescan = true; break; }   // Ctrl+R
                if (k.ch == '?')  { showHelp = true; break; }
                // '/' is the SIGIL that introduces /name, /ext, /dir — it must
                // always reach the query. Treating it as "clear" made the
                // entire query language unreachable, because the very first
                // character of every command was being swallowed. Clearing is
                // done with Backspace or by selecting all, never by '/'.
                query += k.ch;
                break;
            default:
                break;
        }
    }

    // --- shutdown, in the order that matters.
    if (scanner.joinable()) scanner.join();     // no thread outlives our data
    term.restore();

    if (prog.isDone()) Cache::save(root, index);          // cold/fresh scan
    else if (refreshProg && refreshProg->isDone())         // warm refresh done
        Cache::save(root, *refresh);

    std::fputs("\x1b[?25h", stdout);            // show the cursor again
    std::fputs("\r\nGoodbye.\r\n", stdout);
    std::fflush(stdout);
    return 0;
}

// Line-based fallback: identical query language, no ANSI, no raw mode.
static int runLineMode(const std::string& root) {
    FileList index;
    ScanProgress prog;
    std::fprintf(stderr, "Indexing %s ...\n", root.c_str());
    Indexer::run(root, index, prog, 4000000);
    if (index.getCount() == 0) {
        std::fprintf(stderr, "Nothing indexed (root: %s)\n", root.c_str());
        return 1;
    }
    std::fprintf(stderr, "Indexed %d entries. Query syntax: /name /ext /dir /r /dironly. "
                         "Empty line or 'q' to quit.\n", index.getCount());

    ResultSink results;
    Query parser;
    std::string line;
    for (;;) {
        std::fprintf(stderr, "> ");
        if (!std::getline(std::cin, line)) break;
        if (line == "q" || line == "quit" || line == "exit") break;
        if (line.empty()) { results.clear(); continue; }

        Query p;
        if (!p.parse(line)) {
            std::fprintf(stderr, "Cannot parse that query (try /help).\n");
            continue;
        }
        p.run(&index, results);
        std::fprintf(stderr, "%zu match(es):\n", results.size());
        for (const Hit& h : results.get()) {
            const FileNode* n = h.node;
            std::printf("%s\t%s\t%s\n", n->getName().c_str(),
                        n->isDirectory() ? "<dir>" : humanSize(n->getSize()).c_str(),
                        n->getFullPath().c_str());
        }
    }
    Cache::save(root, index);
    return 0;
}

int main(int argc, char** argv) {
    bool noColor = false;
    std::string root;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--no-color" || a == "--nocolor") noColor = true;
        else if (a == "--help" || a == "-h") {
            std::printf("queryfs [path] [--no-color]\n");
            return 0;
        } else if (!a.empty() && a[0] != '-') {
            root = a;
        }
    }
    if (root.empty()) root = defaultRoot();

    Color::init(noColor);
    detectUtf8();

    Terminal term;
    if (Terminal::stdinIsTty())
        return runInteractive(term, root);
    return runLineMode(root);
}