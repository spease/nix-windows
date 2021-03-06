#pragma once

#include "types.hh"
#include "logging.hh"

#include <sys/types.h>
#include <sys/stat.h>
#ifndef _MSC_VER
#include <dirent.h>
#include <unistd.h>
#endif
#include <signal.h>

#include <functional>
#include <limits>
#include <cstdio>
#include <map>
#include <sstream>
#include <optional>
#include <future>

#ifdef _WIN32
#include <iostream>
#endif

#ifndef HAVE_STRUCT_DIRENT_D_TYPE
#define DT_UNKNOWN 0
#define DT_REG 1
#define DT_LNK 2
#define DT_DIR 3
#endif

namespace nix {

struct Sink;
struct Source;


/* The system for which Nix is compiled. */
extern const std::string nativeSystem;


/* Return an environment variable. */
string getEnv(const string & key, const string & def = "");

/* Get the entire environment. */
std::map<std::string, std::string> getEnv();

/* Clear the environment. */
void clearEnv();

/* Return an absolutized path, resolving paths relative to the
   specified directory, or the current directory otherwise.  The path
   is also canonicalised. */
Path absPath(Path path, Path dir = "");

/* Canonicalise a path by removing all `.' or `..' components and
   double or trailing slashes.  Optionally resolves all symlink
   components such that each component of the resulting path is *not*
   a symbolic link. */
Path canonPath(const Path & path, bool resolveSymlinks = false);

/* for paths inside .nar, platform-independent */
Path canonNarPath(const Path & path);

/* Return the directory part of the given canonical path, i.e.,
   everything before the final `/'.  If the path is the root or an
   immediate child thereof (e.g., `/foo'), this means an empty string
   is returned. */
Path dirOf(const Path & path);

/* Return the base name of the given canonical path, i.e., everything
   following the final `/'. */
string baseNameOf(const Path & path);

/* Check whether 'path' is a descendant of 'dir'. */
bool isInDir(const Path & path, const Path & dir);

/* Check whether 'path' is equal to 'dir' or a descendant of 'dir'. */
bool isDirOrInDir(const Path & path, const Path & dir);

/* Get status of `path'. */
// TODO: deprecate on Windows
struct stat lstatPath(const Path & path);

/* Return true iff the given path exists. */
bool pathExists(const Path & path);

/* Read the contents (target) of a symbolic link.  The result is not
   in any way canonicalised. */
Path readLink(const Path & path);

bool isLink(const Path & path);

bool isDirectory(const Path & path);

/* Read the contents of a directory.  The entries `.' and `..' are
   removed. */
#ifndef _WIN32
struct DirEntry
{
    const string name_;
    const ino_t ino;
    const unsigned char type_; // one of DT_*

    const string & name() const { return name_; }
    unsigned char  type() const { return type_; }
    DirEntry(const string & name, ino_t ino, unsigned char type)
        : name_(name), ino(ino), type_(type) { }
};
#else
struct DirEntry
{
          string   name() const { return to_bytes(wfd.cFileName); }
    unsigned char  type() const {
        if (wfd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { return DT_LNK; }
        if (wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY    ) { return DT_DIR; }
        if (wfd.dwFileAttributes & FILE_ATTRIBUTE_NORMAL       ) { return DT_REG; }
        if (wfd.dwFileAttributes & FILE_ATTRIBUTE_ARCHIVE      ) { return DT_REG; }
        std::cerr <<" .type() = DT_UNKNOWN (wfd.dwFileAttributes = "  << wfd.dwFileAttributes << ")" << std::endl;
        return DT_UNKNOWN;
    }

    WIN32_FIND_DATAW wfd;
};
#endif

typedef vector<DirEntry> DirEntries;

DirEntries readDirectory(const Path & path);

unsigned char getFileType(const Path & path);

/* Read the contents of a file into a string. */
#ifndef _WIN32
string readFile(int fd);
#else
string readFile(HANDLE handle);
#endif
string readFile(const Path & path, bool drain = false);
void readFile(const Path & path, Sink & sink);

/* Write a string to a file. */
#ifndef _WIN32
void writeFile(const Path & path, const string & s, mode_t mode = 0666);
void writeFile(const Path & path, Source & source, mode_t mode = 0666);
#else
void writeFile(const Path & path, const string & s, bool setReadOnlyAttribute = false);
void writeFile(const Path & path, Source & source, bool setReadOnlyAttribute = false);
#endif

/* Read a line from a file descriptor. */
#ifndef _WIN32
string readLine(int fd);
#else
string readLine(HANDLE handle);
#endif

/* Write a line to a file descriptor. */
#ifndef _WIN32
void writeLine(int fd, string s);
#else
void writeLine(HANDLE handle, string s);
#endif

/* Delete a path; i.e., in the case of a directory, it is deleted
   recursively. It's not an error if the path does not exist. The
   second variant returns the number of bytes and blocks freed. */
void deletePath(const Path & path);

void deletePath(const Path & path, unsigned long long & bytesFreed);

/* Create a temporary directory. */
Path createTempDir(const Path & tmpRoot = "", const Path & prefix = "nix",
    bool includePid = true, bool useGlobalCounter = true
#ifndef _WIN32
    , mode_t mode = 0755
#endif
    );

std::string getUserName();

/* Return $HOME or the user's home directory from /etc/passwd. */
Path getHome();

/* Return $XDG_CACHE_HOME or $HOME/.cache. */
Path getCacheDir();

/* Return $XDG_CONFIG_HOME or $HOME/.config. */
Path getConfigDir();

/* Return the directories to search for user configuration files */
std::vector<Path> getConfigDirs();

/* Return $XDG_DATA_HOME or $HOME/.local/share. */
Path getDataDir();

/* Create a directory and all its parents, if necessary.  Returns the
   list of created directories, in order of creation. */
Paths createDirs(const Path & path);

/* Create a symlink. */
#ifndef _WIN32
void createSymlink(const Path & target, const Path & link);
#else
enum SymlinkType { SymlinkTypeDangling, SymlinkTypeDirectory, SymlinkTypeFile };
SymlinkType createSymlink(const Path & target, const Path & link);
#endif

/* Atomically create or replace a symlink. */
void replaceSymlink(const Path & target, const Path & link);


/* Wrappers arount read()/write() that read/write exactly the
   requested number of bytes. */
#ifndef _WIN32
void readFull(int fd, unsigned char * buf, size_t count);
void writeFull(int fd, const unsigned char * buf, size_t count, bool allowInterrupts = true);
void writeFull(int fd, const string & s, bool allowInterrupts = true);
#else
void readFull(HANDLE handle, unsigned char * buf, size_t count);
void writeFull(HANDLE handle, const unsigned char * buf, size_t count, bool allowInterrupts = true);
void writeFull(HANDLE handle, const string & s, bool allowInterrupts = true);
#endif

MakeError(EndOfFile, Error);


/* Read a file descriptor until EOF occurs. */
#ifndef _WIN32
string drainFD(int fd, bool block = true);

void drainFD(int fd, Sink & sink, bool block = true);
#else
string drainFD(HANDLE handle/*, bool block = true*/);

void drainFD(HANDLE handle, Sink & sink/*, bool block = true*/);
#endif

/* Automatic cleanup of resources. */


class AutoDelete
{
    Path path;
    bool del;
    bool recursive;
public:
    AutoDelete();
    AutoDelete(const Path & p, bool recursive = true);
    ~AutoDelete();
    void cancel();
    void reset(const Path & p, bool recursive = true);
    operator Path() const { return path; }
};



#ifndef _WIN32
class AutoCloseFD
{
    int fd;
    void close();
public:
    AutoCloseFD();
    AutoCloseFD(int fd);
    AutoCloseFD(const AutoCloseFD & fd) = delete;
    AutoCloseFD(AutoCloseFD&& fd);
    ~AutoCloseFD();
    AutoCloseFD& operator =(const AutoCloseFD & fd) = delete;
    AutoCloseFD& operator =(AutoCloseFD&& fd);
    int get() const;
    explicit operator bool() const;
    int release();
};
#else
class AutoCloseWindowsHandle
{
    HANDLE handle;
    void close();
public:
    AutoCloseWindowsHandle();
    AutoCloseWindowsHandle(HANDLE handle);
    AutoCloseWindowsHandle(const AutoCloseWindowsHandle & fd) = delete;
    AutoCloseWindowsHandle(AutoCloseWindowsHandle&& fd);
    ~AutoCloseWindowsHandle();
    AutoCloseWindowsHandle& operator =(const AutoCloseWindowsHandle & fd) = delete;
    AutoCloseWindowsHandle& operator =(AutoCloseWindowsHandle&& fd);
    HANDLE get() const;
    explicit operator bool() const;
    HANDLE release();
};
#endif


class Pipe
{
public:
#ifndef _WIN32
    AutoCloseFD readSide, writeSide;
#else
    AutoCloseWindowsHandle hRead, hWrite;
#endif
    void create();
};

#ifdef _WIN32
class AsyncPipe
{
public:
    AutoCloseWindowsHandle      hRead, hWrite;
    OVERLAPPED                  overlapped;
    DWORD                       got;
    std::vector<unsigned char>  buffer;
    void create(HANDLE iocp);
};
#endif

#ifndef _WIN32
struct DIRDeleter
{
    void operator()(DIR * dir) const {
        closedir(dir);
    }
};

typedef std::unique_ptr<DIR, DIRDeleter> AutoCloseDir;
#else
struct DIRDeleter
{
    void operator()(HANDLE dir) const {
        FindClose(dir);
    }
};

typedef std::unique_ptr<HANDLE, DIRDeleter> AutoCloseDir;
#endif


#ifndef _WIN32
class Pid
{
    pid_t pid = -1;
    bool separatePG = false;
    int killSignal = SIGKILL;
public:
    Pid();
    Pid(pid_t pid);
    ~Pid();
    void operator =(pid_t pid);
    operator pid_t();
    int kill();
    int wait();
    void setSeparatePG(bool separatePG);
    void setKillSignal(int signal);
    pid_t release();
};

/* Kill all processes running under the specified uid by sending them
   a SIGKILL. */
void killUser(uid_t uid);
#else
class Pid
{
public:
    HANDLE hProcess;
    DWORD dwProcessId;

    Pid();
    ~Pid();
    void set(HANDLE, DWORD);
    int kill();
    int wait();
};
#endif



/* Fork a process that runs the given function, and return the child
   pid to the caller. */
struct ProcessOptions
{
    string errorPrefix = "error: ";
    bool dieWithParent = true;
    bool runExitHandlers = false;
#ifndef _WIN32
    bool allowVfork = true;
#endif
};
#ifndef _WIN32
pid_t startProcess(std::function<void()> fun, const ProcessOptions & options = ProcessOptions());
#endif

struct RunOptions
{
#ifndef _WIN32
    std::optional<uid_t> uid;
    std::optional<uid_t> gid;
    std::optional<Path> chdir;
#endif
    std::optional<std::map<std::string, std::string>> environment;
    Path program;
    bool searchPath = true;
    Strings args; // TODO: unicode on Windows?
    std::optional<std::string> input;
    Source * standardIn = nullptr;
    Sink * standardOut = nullptr;
    bool mergeStderrToStdout = false;
    bool _killStderr = false;

    RunOptions(const Path & program, const Strings & args)
        : program(program), args(args) { };

    RunOptions & killStderr(bool v) { _killStderr = true; return *this; }
};

/* Run a program and return its stdout in a string (i.e., like the
   shell backtick operator). */
string runProgramGetStdout(const RunOptions & options);

string runProgramGetStdout(Path program, bool searchPath = false,
    const Strings & args = Strings(),
    const std::optional<std::string> & input = {});

std::pair<int, std::string> runProgramWithStatus(const RunOptions & options);

/* Run a program without capturing stdout */
void runProgram(Path program, bool searchPath = false,
    const Strings & args = Strings(),
    const std::optional<std::string> & input = {});

/* Run a program, with all setup left to the caller */
void runProgram(const RunOptions & options);

class ExecError : public Error
{
public:
    int status;

    template<typename... Args>
    ExecError(int status, Args... args)
        : Error(args...), status(status)
    { }
};

/* Convert a list of strings to a null-terminated vector of char
   *'s. The result must not be accessed beyond the lifetime of the
   list of strings. */
std::vector<char *> stringsToCharPtrs(const Strings & ss);

/* Close all file descriptors except those listed in the given set.
   Good practice in child processes. */
void closeMostFDs(const set<int> & exceptions);

/* Set the close-on-exec flag for the given file descriptor. */
void closeOnExec(int fd);


/* User interruption. */

extern bool _isInterrupted;

extern thread_local std::function<bool()> interruptCheck;

void setInterruptThrown();

void _interrupted();

void inline checkInterrupt()
{
    if (_isInterrupted || (interruptCheck && interruptCheck()))
        _interrupted();
}

MakeError(Interrupted, BaseError);


MakeError(FormatError, Error);


/* String tokenizer. */
template<class C> C tokenizeString(const string & s, const string & separators = " \t\n\r");


/* Concatenate the given strings with a separator between the
   elements. */
string concatStringsSep(const string & sep, const Strings & ss);
string concatStringsSep(const string & sep, const StringSet & ss);


/* Remove trailing whitespace from a string. */
string chomp(const string & s);


/* Remove whitespace from the start and end of a string. */
string trim(const string & s, const string & whitespace = " \n\r\t");


/* Replace all occurrences of a string inside another string. */
string replaceStrings(const std::string & s,
    const std::string & from, const std::string & to);


std::string rewriteStrings(const std::string & s, const StringMap & rewrites);


/* If a set contains 'from', remove it and insert 'to'. */
template<typename T>
void replaceInSet(std::set<T> & set, const T & from, const T & to)
{
    auto i = set.find(from);
    if (i == set.end()) return;
    set.erase(i);
    set.insert(to);
}


/* Convert the exit status of a child as returned by wait() into an
   error string. */
string statusToString(int status);

bool statusOk(int status);


/* Parse a string into an integer. */
template<class N> bool string2Int(const string & s, N & n)
{
    if (string(s, 0, 1) == "-" && !std::numeric_limits<N>::is_signed)
        return false;
    std::istringstream str(s);
    str >> n;
    return str && str.get() == EOF;
}

/* Parse a string into a float. */
template<class N> bool string2Float(const string & s, N & n)
{
    std::istringstream str(s);
    str >> n;
    return str && str.get() == EOF;
}


/* Return true iff `s' starts with `prefix'. */
bool hasPrefix(const string & s, const string & prefix);


/* Return true iff `s' ends in `suffix'. */
bool hasSuffix(const string & s, const string & suffix);


/* Convert a string to lower case. */
std::string toLower(const std::string & s);


/* Escape a string as a shell word. */
std::string shellEscape(const std::string & s);

#ifdef _WIN32
std::string windowsEscape(const std::string & s);
std::wstring windowsEscapeW(const std::wstring & s);
#endif


/* Exception handling in destructors: print an error message, then
   ignore the exception. */
void ignoreException();


/* Some ANSI escape sequences. */
#ifndef _WIN32
#define ANSI_NORMAL "\x1B[0m"
#define ANSI_BOLD "\x1B[1m"
#define ANSI_FAINT "\x1B[2m"
#define ANSI_RED "\x1B[31;1m"
#define ANSI_GREEN "\x1B[32;1m"
#define ANSI_BLUE "\x1B[34;1m"
#else
#define ANSI_NORMAL ""
#define ANSI_BOLD ""
#define ANSI_FAINT ""
#define ANSI_RED ""
#define ANSI_GREEN ""
#define ANSI_BLUE ""
#endif

/* Truncate a string to 'width' printable characters. If 'filterAll'
   is true, all ANSI escape sequences are filtered out. Otherwise,
   some escape sequences (such as colour setting) are copied but not
   included in the character count. Also, tabs are expanded to
   spaces. */
std::string filterANSIEscapes(const std::string & s,
    bool filterAll = false,
    unsigned int width = std::numeric_limits<unsigned int>::max());


/* Base64 encoding/decoding. */
string base64Encode(const string & s);
string base64Decode(const string & s);


/* Get a value for the specified key from an associate container, or a
   default value if the key doesn't exist. */
template <class T>
string get(const T & map, const string & key, const string & def = "")
{
    auto i = map.find(key);
    return i == map.end() ? def : i->second;
}


/* A callback is a wrapper around a lambda that accepts a valid of
   type T or an exception. (We abuse std::future<T> to pass the value or
   exception.) */
template<typename T>
class Callback
{
    std::function<void(std::future<T>)> fun;
    std::atomic_flag done = ATOMIC_FLAG_INIT;

public:

    Callback(std::function<void(std::future<T>)> fun) : fun(fun) { }

    Callback(Callback && callback) : fun(std::move(callback.fun))
    {
        auto prev = callback.done.test_and_set();
        if (prev) done.test_and_set();
    }

    void operator()(T && t) noexcept
    {
        auto prev = done.test_and_set();
        assert(!prev);
        std::promise<T> promise;
        promise.set_value(std::move(t));
        fun(promise.get_future());
    }

    void rethrow(const std::exception_ptr & exc = std::current_exception()) noexcept
    {
        auto prev = done.test_and_set();
        assert(!prev);
        std::promise<T> promise;
        promise.set_exception(exc);
        fun(promise.get_future());
    }
};

#ifndef _WIN32
/* Start a thread that handles various signals. Also block those signals
   on the current thread (and thus any threads created by it). */
void startSignalHandlerThread();

/* Restore default signal handling. */
void restoreSignals();

struct InterruptCallback
{
    virtual ~InterruptCallback() { };
};

/* Register a function that gets called on SIGINT (in a non-signal
   context). */
std::unique_ptr<InterruptCallback> createInterruptCallback(
    std::function<void()> callback);

void triggerInterrupt();

/* A RAII class that causes the current thread to receive SIGUSR1 when
   the signal handler thread receives SIGINT. That is, this allows
   SIGINT to be multiplexed to multiple threads. */
struct ReceiveInterrupts
{
    pthread_t target;
    std::unique_ptr<InterruptCallback> callback;

    ReceiveInterrupts()
        : target(pthread_self())
        , callback(createInterruptCallback([&]() { pthread_kill(target, SIGUSR1); }))
    { }
};
#endif


/* A RAII helper that increments a counter on construction and
   decrements it on destruction. */
template<typename T>
struct MaintainCount
{
    T & counter;
    long delta;
    MaintainCount(T & counter, long delta = 1) : counter(counter), delta(delta) { counter += delta; }
    ~MaintainCount() { counter -= delta; }
};


/* Return the number of rows and columns of the terminal. */
std::pair<unsigned short, unsigned short> getWindowSize();


/* Used in various places. */
typedef std::function<bool(const Path & path)> PathFilter;

extern PathFilter defaultPathFilter;

#ifdef _WIN32
std::wstring getCwdW();
std::wstring getArgv0W();
std::wstring getEnvW(const std::wstring & key, const std::wstring & def);

struct no_case_compare {
    bool operator() (const std::wstring & s1, const std::wstring & s2) const {
        return wcsicmp(s1.c_str(), s2.c_str()) < 0;
    }
};
std::map<std::wstring, std::wstring, no_case_compare> getEntireEnvW();

#if _WIN32_WINNT >= 0x0600
Path handleToPath(HANDLE handle);
std::wstring handleToFileName(HANDLE handle);
#endif

std::wstring pathW(const Path & path);
inline bool isslash(int c) { return c == '/' || c == '\\'; }
#else
inline bool isslash(int c) { return c == '/'; }
#endif


#ifndef _WIN32
/* Create a Unix domain socket in listen mode. */
AutoCloseFD createUnixDomainSocket(const Path & path, mode_t mode);
#endif


}
