// pkg_test.h: eskiuc test, the package's test runner (internal to pkg.cpp, which includes
// it once, inside its anonymous namespace). Each tests/NAME.esk is compiled to
// build/tests/NAME and run, N at a time; build and run output go to build/tests/NAME.log.
// Statuses: pass (exit 0), fail (another exit code), build-failed, timeout (killed after
// --timeout seconds), crashed (killed by a signal). docs/lang/build.md has the flags and
// the --json schema.

struct TestOptions {
    std::vector<std::string> filters, flags;
    unsigned jobs = 0;
    double timeout = 60;
    bool json = false;
};

struct TestResult {
    std::string name, source, log, status;
    int exitCode = 0;
    bool hasExitCode = false;
    int signal = 0;
    double buildMs = 0, runMs = 0;
    bool ran = false;
};

// Half the cores (at least one): each job is a compiler, then the test, and an optimizing
// compile is the heavy part; the other half keeps the machine usable.
unsigned defaultTestJobs() {
    unsigned n = std::thread::hardware_concurrency();
    return n >= 2 ? n / 2 : 1;
}

TestOptions parseTestArgs(const std::vector<std::string>& args) {
    TestOptions o;
    auto value = [&](size_t& i, const std::string& opt) -> std::string {
        if (i + 1 >= args.size()) throw PkgUsage(opt + " needs a value");
        return args[++i];
    };
    auto number = [](const std::string& opt, const std::string& v, double lo, double hi) {
        char* end = nullptr;
        double d = std::strtod(v.c_str(), &end);
        if (v.empty() || *end || !(d >= lo && d <= hi))
            throw PkgUsage(opt + " needs a number from " + std::to_string((int)lo) + " to " +
                           std::to_string((int)hi) + ", not '" + v + "'");
        return d;
    };
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--json") o.json = true;
        else if (a == "-j" || a == "--jobs") o.jobs = (unsigned)number(a, value(i, a), 1, 1024);
        else if (a.rfind("--jobs=", 0) == 0) o.jobs = (unsigned)number("--jobs", a.substr(7), 1, 1024);
        else if (a.size() > 2 && a.rfind("-j", 0) == 0) o.jobs = (unsigned)number("-j", a.substr(2), 1, 1024);
        else if (a == "--timeout") o.timeout = number(a, value(i, a), 0, 86400);
        else if (a.rfind("--timeout=", 0) == 0) o.timeout = number("--timeout", a.substr(10), 0, 86400);
        else if (a == "-o" || a.rfind("-o=", 0) == 0) throw PkgUsage("-o: each test is built to build/tests/NAME");
        else if (!a.empty() && a[0] == '-') {
            size_t first = i;
            checkCompilerFlag(args, i);
            for (size_t k = first; k <= i; ++k) o.flags.push_back(args[k]);
        } else {
            o.filters.push_back(a);
        }
    }
    if (o.jobs == 0) o.jobs = defaultTestJobs();
    return o;
}

// Whether the test `stem` is selected: no filter, a filter it contains, a glob it matches
// (`*`, `?`, `[...]`), or a path whose file is tests/STEM.esk.
bool testSelected(const std::string& stem, const std::vector<std::string>& filters) {
    if (filters.empty()) return true;
    for (std::string f : filters) {
        if (endsWithEsk(f)) {
            if (fs::path(f).stem().string() == stem) return true;
            continue;
        }
        if (f.find_first_of("*?[") != std::string::npos) {
            auto pat = llvm::GlobPattern::create(f);
            if (!pat) llvm::consumeError(pat.takeError());
            else if (pat->match(stem)) return true;
            continue;
        }
        if (stem.find(f) != std::string::npos) return true;
    }
    return false;
}

struct ChildOutcome {
    bool launched = false, timedOut = false;
    int exitCode = 0, signal = 0;
    double ms = 0;
};

// Run `exe args` with stdin from the null device and stdout and stderr appended to `log`;
// kill it after `timeout` seconds (0: no limit).
ChildOutcome runChild(const std::string& exe, const std::vector<std::string>& args,
                      const std::string& log, double timeout) {
    ChildOutcome r;
    auto start = std::chrono::steady_clock::now();
    auto elapsed = [&] {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    };
#ifndef _WIN32
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(exe.c_str()));
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    pid_t pid = 0;
    int err = posix_spawn(&pid, exe.c_str(), &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (err != 0) {
        std::ofstream(log, std::ios::app) << "error: could not run " << exe << ": " << std::strerror(err) << "\n";
        return r;
    }
    r.launched = true;
    int status = 0;
    if (timeout <= 0) {
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    } else {
        int sleepUs = 1000;
        while (true) {
            pid_t w = waitpid(pid, &status, WNOHANG);
            if (w == pid || (w < 0 && errno != EINTR)) break;
            if (elapsed() >= timeout * 1000) {
                kill(pid, SIGKILL);
                while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
                r.timedOut = true;
                break;
            }
            usleep(sleepUs);
            if (sleepUs < 20000) sleepUs *= 2;
        }
    }
    r.ms = elapsed();
    if (r.timedOut) return r;
    if (WIFSIGNALED(status)) r.signal = WTERMSIG(status);
    else if (WIFEXITED(status)) r.exitCode = WEXITSTATUS(status);
    else r.exitCode = 1;
#else
    std::vector<llvm::StringRef> argv = {exe};
    for (const auto& a : args) argv.push_back(a);
    std::optional<llvm::StringRef> redirects[] = {llvm::StringRef(""), llvm::StringRef(log), llvm::StringRef(log)};
    std::string errMsg;
    bool failed = false;
    llvm::sys::ProcessInfo pi = llvm::sys::ExecuteNoWait(exe, argv, std::nullopt, redirects, 0, &errMsg, &failed);
    if (failed) {
        std::ofstream(log, std::ios::app) << "error: could not run " << exe << ": " << errMsg << "\n";
        return r;
    }
    r.launched = true;
    std::optional<unsigned> wait;
    if (timeout > 0) wait = (unsigned)std::ceil(timeout);
    llvm::sys::ProcessInfo done = llvm::sys::Wait(pi, wait, &errMsg);
    r.ms = elapsed();
    if (done.ReturnCode == -2 && timeout > 0 && r.ms >= timeout * 1000) r.timedOut = true;
    else if (done.ReturnCode == -2) r.signal = -1;
    else r.exitCode = done.ReturnCode;
#endif
    return r;
}

std::string signalName(int sig) {
#ifndef _WIN32
    switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGABRT: return "SIGABRT";
    case SIGBUS: return "SIGBUS";
    case SIGFPE: return "SIGFPE";
    case SIGILL: return "SIGILL";
    case SIGTRAP: return "SIGTRAP";
    case SIGKILL: return "SIGKILL";
    case SIGTERM: return "SIGTERM";
    default: break;
    }
#endif
    return sig > 0 ? "signal " + std::to_string(sig) : "an exception";
}

TestResult runOneTest(const std::string& self, const std::string& source, const std::string& outDir,
                      const TestOptions& o) {
    TestResult t;
    t.source = source;
    t.name = fs::path(source).stem().string();
    std::string exe = outDir + "/" + t.name;
#ifdef _WIN32
    exe += ".exe";
#endif
    t.log = outDir + "/" + t.name + ".log";
    std::error_code ec;
    fs::remove(t.log, ec);
    std::vector<std::string> cargs = {source, "-o", exe};
    cargs.insert(cargs.end(), o.flags.begin(), o.flags.end());
    ChildOutcome b = runChild(self, cargs, t.log, 0);
    t.buildMs = b.ms;
    if (!b.launched || b.signal || b.exitCode != 0) {
        t.status = "build-failed";
        t.hasExitCode = b.launched && !b.signal;
        t.exitCode = b.exitCode;
        t.signal = b.signal;
        return t;
    }
    ChildOutcome r = runChild(exe, {}, t.log, o.timeout);
    t.ran = true;
    t.runMs = r.ms;
    if (!r.launched) {
        t.status = "fail";
        t.hasExitCode = true;
        t.exitCode = 127;
    } else if (r.timedOut) {
        t.status = "timeout";
    } else if (r.signal) {
        t.status = "crashed";
        t.signal = r.signal;
    } else {
        t.hasExitCode = true;
        t.exitCode = r.exitCode;
        t.status = r.exitCode == 0 ? "pass" : "fail";
    }
    return t;
}

std::string jsonText(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += (char)c; }
        else if (c == '\n') out += "\\n";
        else if (c == '\t') out += "\\t";
        else if (c == '\r') out += "\\r";
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); out += b; }
        else out += (char)c;
    }
    return out + "\"";
}

std::string msText(double ms) {
    char b[32];
    std::snprintf(b, sizeof b, "%.1f", ms);
    return b;
}

std::string testJson(const TestResult& t) {
    std::string s = "{\"type\":\"test\",\"name\":" + jsonText(t.name) + ",\"source\":" + jsonText(t.source) +
                    ",\"status\":\"" + t.status + "\",\"exit_code\":" +
                    (t.hasExitCode ? std::to_string(t.exitCode) : "null") + ",\"signal\":" +
                    (t.signal > 0 ? std::to_string(t.signal) : "null") + ",\"build_ms\":" + msText(t.buildMs) +
                    ",\"run_ms\":" + (t.ran ? msText(t.runMs) : "null") + ",\"log\":" + jsonText(t.log) + "}";
    return s;
}

// The human line of a finished test, and the end of its log when it did not pass.
void printTestHuman(const TestResult& t) {
    if (t.status == "pass") {
        std::cout << "PASS " << t.name << std::endl;
        return;
    }
    std::cout << "FAIL " << t.name << " (";
    if (t.status == "build-failed") std::cout << "build failed";
    else if (t.status == "timeout") std::cout << "timeout after " << msText(t.runMs / 1000) << " s";
    else if (t.status == "crashed") std::cout << "crashed: " << signalName(t.signal);
    else std::cout << "exit " << t.exitCode;
    std::cout << ")" << std::endl;
    std::ifstream in(t.log);
    std::vector<std::string> lines;
    for (std::string l; std::getline(in, l);) lines.push_back(l);
    const size_t keep = 40;
    if (lines.size() > keep) std::cout << "    ... (" << lines.size() - keep << " more lines in " << displayPath(t.log) << ")\n";
    for (size_t i = lines.size() > keep ? lines.size() - keep : 0; i < lines.size(); ++i)
        std::cout << "    " << lines[i] << "\n";
    std::cout.flush();
}

int cmdTest(const char* argv0, const std::vector<std::string>& args) {
    TestOptions o = parseTestArgs(args);
    Manifest m = manifestFromCwd();
    resolveAndLock(m, false);
    std::vector<std::string> tests;
    size_t found = 0;
    std::error_code ec;
    for (const auto& ent : fs::directory_iterator(m.dir + "/tests", ec))
        if (ent.path().extension() == ".esk" && ent.is_regular_file(ec)) {
            ++found;
            if (testSelected(ent.path().stem().string(), o.filters)) tests.push_back(ent.path().generic_string());
        }
    std::sort(tests.begin(), tests.end());
    if (found == 0) {
        if (o.json) std::cout << "{\"type\":\"summary\",\"total\":0,\"passed\":0,\"failed\":0,\"build_failed\":0,\"timeout\":0,\"crashed\":0,\"duration_ms\":0.0,\"jobs\":" << o.jobs << "}" << std::endl;
        else std::cout << "no tests in " << displayPath(m.dir + "/tests") << std::endl;
        return 0;
    }
    if (tests.empty()) {
        std::string f;
        for (const auto& x : o.filters) f += (f.empty() ? "'" : ", '") + x + "'";
        std::cerr << "error: no test in " << displayPath(m.dir + "/tests") << " matches " << f << std::endl;
        return 1;
    }
    std::string outDir = m.dir + "/build/tests";
    fs::create_directories(outDir, ec);
    std::string self = selfExe(argv0);
    auto start = std::chrono::steady_clock::now();
    std::vector<TestResult> results(tests.size());
    std::atomic<size_t> next{0};
    std::mutex printLock;
    auto worker = [&] {
        for (size_t i; (i = next++) < tests.size();) {
            results[i] = runOneTest(self, tests[i], outDir, o);
            std::lock_guard<std::mutex> g(printLock);
            if (o.json) std::cout << testJson(results[i]) << std::endl;
            else printTestHuman(results[i]);
        }
    };
    unsigned n = std::min<unsigned>(o.jobs, (unsigned)tests.size());
    std::vector<std::thread> pool;
    for (unsigned k = 1; k < n; ++k) pool.emplace_back(worker);
    worker();
    for (auto& th : pool) th.join();
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::map<std::string, int> count;
    for (const auto& r : results) count[r.status]++;
    int failed = (int)tests.size() - count["pass"];
    if (o.json) {
        std::cout << "{\"type\":\"summary\",\"total\":" << tests.size() << ",\"passed\":" << count["pass"]
                  << ",\"failed\":" << failed << ",\"build_failed\":" << count["build-failed"]
                  << ",\"timeout\":" << count["timeout"] << ",\"crashed\":" << count["crashed"]
                  << ",\"duration_ms\":" << msText(ms) << ",\"jobs\":" << n << "}" << std::endl;
    } else {
        std::cout << count["pass"] << " passed, " << failed << " failed";
        std::string detail;
        for (const char* s : {"build-failed", "timeout", "crashed"})
            if (count[s]) detail += (detail.empty() ? "" : ", ") + std::to_string(count[s]) + " " + s;
        if (!detail.empty()) std::cout << " (" << detail << ")";
        std::cout << " in " << msText(ms / 1000) << " s, " << n << (n == 1 ? " job" : " jobs") << std::endl;
        if (failed) {
            std::cout << "failed:";
            for (const auto& r : results)
                if (r.status != "pass") std::cout << " " << r.name;
            std::cout << std::endl;
        }
    }
    return failed ? 1 : 0;
}
