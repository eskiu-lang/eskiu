// `eskiuc --emit-header`: the C declarations of a program's `export` functions and of the
// types they reach. Records keep their declared field types (bitfields, unnamed bitfields,
// `packed` and `pack(N)` through `#pragma pack`), so a C compiler lays them out as Eskiu
// does (docs/dev/abi.md). Mirrored by the self-hosted `sema_c_header` (selfhost/sema_header.esk),
// byte for byte.
#include "type_checker.h"
#include "../ast/type_qual.h"
#include <algorithm>
#include <cctype>
#include <sstream>

struct CHeaderWriter {
    TypeChecker& tc;
    std::string error;
    std::vector<std::string> records;           // definition order: by-value fields first
    std::set<std::string> done, visiting;
    std::vector<std::string> queued;            // reached through a pointer: defined later
    std::vector<std::string> enums;             // plain enums, in first-use order
    std::set<std::string> enumSeen;
    std::map<std::string, std::string> recordText;   // record name -> its definition

    explicit CHeaderWriter(TypeChecker& t) : tc(t) {}

    void fail(const std::string& what) {
        if (error.empty()) error = what;
    }

    static std::string intName(const std::string& n) {
        if (n == "int" || n == "int32") return "int32_t";
        if (n == "uint" || n == "uint32") return "uint32_t";
        if (n == "int8" || n == "int16" || n == "int64" || n == "uint8" || n == "uint16" || n == "uint64")
            return n + "_t";
        return "";
    }

    // The C spelling of `raw` as a declarator around `name` (`int32_t name[4]`), queueing
    // the records it names: a record by value is defined before the one that holds it.
    std::string decl(const std::string& raw, const std::string& name, bool byValue) {
        std::string bare = tyq::strip(raw);
        if (tc.plainEnumDecls.count(bare)) return withName(useEnum(bare), name);
        ty::Type t = ty::Type::parse(tc.normalizeType(bare));
        std::string dims;
        while (t.kind == ty::Type::Kind::Array && t.elem) {
            dims += "[" + dimText(t.dim) + "]";
            ty::Type e = *t.elem;
            t = e;
        }
        return withName(spell(t, byValue), name) + dims;
    }

    static std::string withName(const std::string& base, const std::string& name) {
        return name.empty() ? base : base + " " + name;
    }

    std::string dimText(const std::string& dim) {
        long long v = 0;
        if (!dim.empty() && std::all_of(dim.begin(), dim.end(), [](unsigned char c) { return std::isdigit(c); }))
            return dim;
        if (auto ec = tc.enumConstants.find(dim); ec != tc.enumConstants.end()) return std::to_string(ec->second);
        bool ok = ty::foldDim(dim, [&](const std::string& n, long long& r) {
            if (auto ec = tc.enumConstants.find(n); ec != tc.enumConstants.end()) { r = ec->second; return true; }
            const TypeChecker::Symbol* s = tc.findSymbol(n);
            return s && s->isConst && s->constInit && tc.foldConstInt(s->constInit, r);
        }, v);
        if (!ok) fail("the array size '" + dim + "' cannot be written in a C header");
        return std::to_string(v);
    }

    std::string useEnum(const std::string& name) {
        if (enumSeen.insert(name).second) enums.push_back(name);
        return name;
    }

    std::string spell(const ty::Type& t, bool byValue) {
        using K = ty::Type::Kind;
        switch (t.kind) {
            case K::Void: return "void";
            case K::Bool: return "bool";
            case K::Char: return "char";
            case K::String: return "char*";
            case K::Float: return t.name;
            case K::Int: {
                std::string c = intName(t.name);
                if (c.empty()) fail("the type '" + t.name + "' has no C spelling");
                return c;
            }
            case K::Pointer: {
                if (!t.pointee || t.pointee->kind == K::Void || t.pointee->kind == K::Array) return "void*";
                if (t.pointee->kind == K::Named && tc.plainEnumDecls.count(t.pointee->name))
                    return useEnum(t.pointee->name) + "*";
                return spell(*t.pointee, false) + "*";
            }
            case K::Fn: return "struct { void* fn; void* env; }";
            case K::Slice: return "struct { void* ptr; int64_t len; }";
            case K::Interface: return "struct { void* data; void* vtable; }";
            case K::Struct: case K::Named: {
                std::string n = t.nominalName();
                if (tc.plainEnumDecls.count(n)) return useEnum(n);
                if (tc.templateInstanceArgs.count(n)) {
                    fail("a generic instance has no stable C name (wrap it in a named struct)");
                    return n;
                }
                if (tc.adtEnums.count(n)) {
                    fail("the sum type '" + n + "' has no C layout");
                    return n;
                }
                if (tc.interfaceDecls.count(n)) return "struct { void* data; void* vtable; }";
                if (!tc.structs.count(n)) {
                    fail("the type '" + n + "' has no C spelling");
                    return n;
                }
                if (byValue) record(n);
                else if (!done.count(n) && !visiting.count(n)) queued.push_back(n);
                return n;
            }
            default:
                fail("the type '" + t.str() + "' has no C spelling");
                return "void";
        }
    }

    void record(const std::string& name) {
        if (done.count(name) || visiting.count(name)) return;
        visiting.insert(name);
        const TypeChecker::StructInfo& si = tc.structs.at(name);
        std::ostringstream os;
        int pack = si.packAlign;
        if (pack > 0) os << "#pragma pack(push, " << pack << ")\n";
        os << (si.isUnion ? "union " : "struct ") << name << " {\n";
        size_t pad = 0;
        for (size_t i = 0; i <= si.fields.size(); ++i) {
            for (; pad < si.pads.size() && si.pads[pad].before == i; ++pad)
                os << "    " << decl(si.pads[pad].type, "", true) << " : "
                   << std::max(si.pads[pad].bitWidth, 0) << ";\n";
            if (i == si.fields.size()) break;
            const auto& f = si.fields[i];
            os << "    " << decl(f.type, f.name, true);
            if (f.bitWidth > 0) os << " : " << f.bitWidth;
            os << ";\n";
        }
        os << "};\n";
        if (pack > 0) os << "#pragma pack(pop)\n";
        visiting.erase(name);
        done.insert(name);
        records.push_back(name);
        recordText[name] = os.str();
    }

    std::string write(Program* program, const std::string& source, const std::string& guard) {
        std::ostringstream protos;
        for (auto& d : program->declarations) {
            auto* f = nodeCast<FunctionDecl>(d.get());
            if (!f || !f->isExport || !f->body) continue;
            std::string ps;
            for (const auto& p : f->params) ps += (ps.empty() ? "" : ", ") + decl(p.first, p.second, true);
            protos << decl(f->returnType, f->name, true) << "(" << (ps.empty() ? "void" : ps) << ");\n";
        }
        for (size_t i = 0; i < queued.size(); ++i) record(queued[i]);
        std::ostringstream os;
        os << "/* Generated by eskiuc --emit-header from " << source << ". Do not edit. */\n"
           << "#ifndef " << guard << "\n#define " << guard << "\n\n"
           << "#include <stdbool.h>\n#include <stdint.h>\n\n"
           << "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n";
        if (!records.empty()) {
            for (const auto& r : records)
                os << "typedef " << (tc.structs.at(r).isUnion ? "union " : "struct ") << r << " " << r << ";\n";
            os << "\n";
        }
        for (const auto& e : enums) {
            os << "typedef int32_t " << e << ";\nenum {";
            const auto* ed = tc.plainEnumDecls.at(e);
            for (size_t i = 0; i < ed->members.size(); ++i)
                os << (i ? "," : "") << "\n    " << ed->members[i].first << " = " << ed->members[i].second;
            os << "\n};\n\n";
        }
        for (const auto& r : records) os << recordText[r] << "\n";
        os << protos.str();
        os << "\n#ifdef __cplusplus\n}\n#endif\n\n#endif\n";
        return os.str();
    }
};

std::string TypeChecker::cHeader(Program* program, const std::string& source, const std::string& guard,
                                 std::string& error) {
    CHeaderWriter w(*this);
    std::string text = w.write(program, source, guard);
    error = w.error;
    return text;
}
