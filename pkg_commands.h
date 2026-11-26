// pkg_commands.h: eskiuc new, add and fetch (internal to pkg.cpp, which includes it once,
// inside its anonymous namespace).

int cmdNew(const std::vector<std::string>& args) {
    for (const auto& a : args)
        if (a.size() > 1 && a[0] == '-') throw PkgUsage("unknown option '" + a + "'");
    if (args.size() != 1) throw PkgUsage(args.empty() ? "missing the package NAME" : "takes one NAME");
    const std::string& name = args[0];
    if (!validName(name)) throw PkgError("invalid package name '" + name + "' (letters, digits and '_')");
    std::error_code ec;
    if (fs::exists(name, ec)) throw PkgError("'" + name + "' already exists");
    fs::create_directories(name + "/src", ec);
    if (ec) throw PkgError("cannot create " + name + "/src: " + ec.message());
    bool ok = writeText(name + "/eskiu.toml",
                        "[package]\nname = \"" + name + "\"\nversion = \"0.1.0\"\n"
                        "main = \"src/main.esk\"\n\n[deps]\n") &&
              writeText(name + "/src/main.esk",
                        "import <io>;\n\nint main() {\n    println(\"Hello from " + name +
                            "!\");\n    return 0;\n}\n") &&
              writeText(name + "/.gitignore", "/build/\n");
    if (!ok) throw PkgError("cannot write the files of " + name);
    std::cout << "created package '" << name << "' in " << name << "/" << std::endl;
    return 0;
}

std::string tomlString(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}

int cmdAdd(const std::vector<std::string>& args) {
    std::string name, git, rev, path;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= args.size()) throw PkgUsage(a + " needs a value");
            return args[++i];
        };
        if (a == "--git") git = next();
        else if (a == "--rev") rev = next();
        else if (a == "--path") path = next();
        else if (!a.empty() && a[0] == '-') throw PkgUsage("unknown option '" + a + "'");
        else if (name.empty()) name = a;
        else throw PkgUsage("unexpected argument '" + a + "'");
    }
    if (name.empty()) throw PkgUsage("missing the package NAME");
    if (git.empty() == path.empty()) throw PkgUsage("needs one of --git URL and --path DIR");
    if (!rev.empty() && git.empty()) throw PkgUsage("--rev goes with --git");
    if (!validName(name)) throw PkgError("invalid package name '" + name + "' (letters, digits and '_')");
    Manifest m = manifestFromCwd();
    if (name == m.name) throw PkgError("a package cannot depend on itself ('" + name + "')");
    std::string entry = name + " = { ";
    if (!git.empty()) {
        entry += "git = " + tomlString(git);
        if (!rev.empty()) entry += ", rev = " + tomlString(rev);
    } else {
        // A relative path is written relative to the manifest, as it is read.
        std::error_code ec;
        fs::path p(path);
        if (p.is_relative()) {
            fs::path rel = fs::relative(fs::weakly_canonical(fs::absolute(p, ec), ec), m.dir, ec);
            if (!ec && !rel.empty()) p = rel;
        }
        entry += "path = " + tomlString(p.generic_string());
    }
    entry += " }";

    std::string file = m.dir + "/eskiu.toml";
    std::string old = readText(file);
    std::vector<std::string> lines;
    {
        std::istringstream in(old);
        for (std::string l; std::getline(in, l);) lines.push_back(l);
    }
    auto trimmed = [](const std::string& s) {
        size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
        return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    };
    int header = -1;
    for (size_t i = 0; i < lines.size(); ++i)
        if (trimmed(lines[i]) == "[deps]") { header = (int)i; break; }
    if (header < 0) {
        if (!lines.empty() && !trimmed(lines.back()).empty()) lines.push_back("");
        lines.push_back("[deps]");
        lines.push_back(entry);
    } else {
        int insertAt = header + 1, replace = -1;
        for (size_t i = header + 1; i < lines.size(); ++i) {
            std::string t = trimmed(lines[i]);
            if (!t.empty() && t[0] == '[') break;
            if (t.empty() || t[0] == '#') continue;
            insertAt = (int)i + 1;
            size_t k = t.find_first_of(" \t=");
            if (t.substr(0, k) == name) replace = (int)i;
        }
        if (replace >= 0) lines[replace] = entry;
        else lines.insert(lines.begin() + insertAt, entry);
    }
    std::string text;
    for (const auto& l : lines) text += l + "\n";
    if (!writeText(file, text)) throw PkgError("cannot write " + displayPath(file));
    try {
        auto entries = resolveAndLock(readManifest(m.dir), false);
        for (const auto& e : entries)
            if (e.name == name)
                std::cout << "added " << name << " ("
                          << (e.kind == "git" ? "commit " + e.commit.substr(0, 12) : e.path) << ")"
                          << std::endl;
    } catch (...) {
        writeText(file, old);
        throw;
    }
    return 0;
}

int cmdFetch(const std::vector<std::string>& args) {
    if (!args.empty())
        throw PkgUsage(args[0][0] == '-' ? "unknown option '" + args[0] + "'"
                                         : "unexpected argument '" + args[0] + "'");
    Manifest m = manifestFromCwd();
    for (const auto& e : resolveAndLock(m, true)) {
        if (e.kind == "root") continue;
        std::cout << e.name << "\t" << (e.kind == "git" ? e.commit : e.path) << std::endl;
    }
    fetchNatives(m.dir);
    return 0;
}
