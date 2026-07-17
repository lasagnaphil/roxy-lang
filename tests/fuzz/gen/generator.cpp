#include "generator.hpp"

#include <cassert>
#include <cstdio>

namespace rx::gen {

GenConfig GenConfig::fuzz_default() {
    return GenConfig{};  // the member initializers are the fuzz shape
}

GenConfig GenConfig::benchmark_default() {
    GenConfig config;
    config.num_modules = 50;
    config.min_funcs_per_module = 6;
    config.max_funcs_per_module = 14;
    config.max_structs_per_module = 4;
    config.max_enums_per_module = 2;
    config.max_methods_per_struct = 3;
    config.max_stmts_per_block = 8;
    config.max_expr_depth = 4;
    config.max_params = 4;
    // Realism cap, not a compiler workaround: real functions rarely exceed
    // ~50 statements, and benchmark corpora scale by breadth (functions x
    // modules), not per-function extremity.
    config.max_stmts_per_function = 50;
    config.allow_print = true;
    return config;
}

namespace {

// ── Type model ─────────────────────────────────────────────────────────────
// The generator's own lightweight mirror of Roxy's type system: just enough
// to direct expression generation. Struct/enum indices are global across
// modules (each records its owning module).

struct GType {
    enum Kind : uint8_t { Bool, I32, I64, F32, F64, Str, StructT, EnumT };
    Kind kind = I32;
    uint32_t index = 0;  // structs[index] / enums[index] for StructT / EnumT

    bool operator==(const GType& other) const {
        return kind == other.kind && index == other.index;
    }
    bool is_numeric() const {
        return kind == I32 || kind == I64 || kind == F32 || kind == F64;
    }
    bool is_prim() const { return kind != StructT && kind != EnumT; }
};

struct FieldInfo {
    std::string name;
    GType type;
};

struct StructInfo {
    std::string name;
    uint32_t module_idx;
    std::vector<FieldInfo> fields;
};

struct EnumInfo {
    std::string name;
    uint32_t module_idx;
    std::vector<std::string> variants;
};

struct FuncInfo {
    std::string name;
    uint32_t module_idx;
    std::vector<GType> params;
    GType ret;
    bool has_ret = false;
    int receiver = -1;    // global struct index when this is a method
    bool is_pub = false;  // primitive-only signature, callable cross-module
    uint64_t cost = 1;    // estimated dynamic ops per invocation (incl. callees)
};

// `fun name<T>(v: T): T { return v; }` — instantiable with any concrete type,
// so it is the cheapest way to stress monomorphization from call sites.
struct GenericFnInfo {
    std::string name;
    uint32_t module_idx;
};

// `struct Name<T> { value: T; }` — instantiated via field-value inference.
struct GenericStructInfo {
    std::string name;
    uint32_t module_idx;
};

struct VarInfo {
    std::string name;  // may be a path like "self.hp" inside methods
    GType type;
    bool assignable;
};

// ── Generator ──────────────────────────────────────────────────────────────

struct Generator {
    Entropy& ent;
    const GenConfig& cfg;
    GenStats stats;

    std::vector<StructInfo> structs;
    std::vector<EnumInfo> enums;
    std::vector<FuncInfo> funcs;
    std::vector<GenericFnInfo> generic_fns;
    std::vector<GenericStructInfo> generic_structs;
    std::vector<std::string> module_names;

    // Per-module state
    uint32_t cur_module = 0;
    std::vector<uint32_t> imports;       // module indices imported by cur module
    std::vector<size_t> from_imports;    // func indices callable unqualified

    // Per-function state
    size_t cur_func = 0;  // index of the function whose body is being generated
    bool cur_has_ret = false;
    GType cur_ret;
    std::vector<VarInfo> scope;
    uint32_t block_depth = 0;
    uint32_t stmt_budget = 0;   // remaining statements for the current function
    uint64_t fn_cost = 0;       // accumulated dynamic cost of the current body
    uint64_t cost_scale = 1;    // product of enclosing loop trip counts

    static uint64_t clamped_mul(uint64_t a, uint64_t b) {
        constexpr uint64_t CAP = 1ull << 40;
        if (b != 0 && a > CAP / b) return CAP;
        return a * b;
    }

    // True if invoking `func` here (under the current loop nesting) still fits
    // the per-function dynamic budget.
    bool affordable(const FuncInfo& func) const {
        return fn_cost + clamped_mul(func.cost, cost_scale) <= cfg.max_dynamic_cost;
    }

    // Emission state
    std::string out;
    uint32_t indent = 0;
    uint32_t name_counter = 0;

    Generator(Entropy& entropy, const GenConfig& config) : ent(entropy), cfg(config) {}

    // ── Names ──
    // Varied-length syllable names + a unique numeric suffix: uniqueness is
    // guaranteed by the counter, while the varied prefixes keep identifier
    // interning/hashing realistic (unlike `f1, f2, f3, ...`).

    std::string make_word() {
        static const char* SYLLABLES[] = {
            "al", "bar", "cor", "del", "eng", "fal", "gor", "hin", "ir",
            "jak", "kel", "lum", "mor", "nis", "or", "pel", "quor", "ren",
            "sol", "tan", "ul", "vor", "wex", "xan", "yor", "zel",
        };
        constexpr uint32_t SYLLABLE_COUNT = sizeof(SYLLABLES) / sizeof(SYLLABLES[0]);
        uint32_t syllable_count = 2 + ent.range(3);
        std::string word;
        for (uint32_t i = 0; i < syllable_count; i++) {
            word += SYLLABLES[ent.range(SYLLABLE_COUNT)];
        }
        return word;
    }

    std::string fresh_name(bool capitalized) {
        std::string name = make_word();
        if (capitalized) name[0] = static_cast<char>(name[0] - 'a' + 'A');
        name += "_";
        name += std::to_string(name_counter++);
        return name;
    }

    // ── Emission helpers ──

    void line(const std::string& text) {
        for (uint32_t i = 0; i < indent; i++) out += "    ";
        out += text;
        out += '\n';
        stats.lines++;
    }

    void open(const std::string& head) {
        line(head + " {");
        indent++;
    }

    void close() {
        assert(indent > 0);
        indent--;
        line("}");
    }

    // ── Type helpers ──

    std::string type_name(GType type) const {
        switch (type.kind) {
            case GType::Bool: return "bool";
            case GType::I32:  return "i32";
            case GType::I64:  return "i64";
            case GType::F32:  return "f32";
            case GType::F64:  return "f64";
            case GType::Str:  return "string";
            case GType::StructT: return structs[type.index].name;
            case GType::EnumT:   return enums[type.index].name;
        }
        return "i32";
    }

    GType random_prim_type() {
        // Weighted toward integers: they compose with everything (checksums,
        // loop bounds, comparisons) and match real code's distribution.
        static const GType::Kind KINDS[] = {
            GType::I32, GType::I32, GType::I32, GType::I64,
            GType::F32, GType::F64, GType::Bool, GType::Str,
        };
        return GType{KINDS[ent.range(8)], 0};
    }

    // Module-local structs/enums declared so far (usable for fields, vars).
    std::vector<uint32_t> local_structs() const {
        std::vector<uint32_t> result;
        for (uint32_t i = 0; i < structs.size(); i++) {
            if (structs[i].module_idx == cur_module) result.push_back(i);
        }
        return result;
    }

    std::vector<uint32_t> local_enums() const {
        std::vector<uint32_t> result;
        for (uint32_t i = 0; i < enums.size(); i++) {
            if (enums[i].module_idx == cur_module) result.push_back(i);
        }
        return result;
    }

    GType random_var_type() {
        // Mostly primitives, sometimes a module-local struct or enum.
        if (cfg.max_structs_per_module > 0 && ent.chance(20)) {
            auto candidates = local_structs();
            if (!candidates.empty()) {
                return GType{GType::StructT, candidates[ent.range(static_cast<uint32_t>(candidates.size()))]};
            }
        }
        if (cfg.max_enums_per_module > 0 && ent.chance(15)) {
            auto candidates = local_enums();
            if (!candidates.empty()) {
                return GType{GType::EnumT, candidates[ent.range(static_cast<uint32_t>(candidates.size()))]};
            }
        }
        return random_prim_type();
    }

    // ── Literals ──

    std::string int_literal_nonzero(GType::Kind kind) {
        std::string text = std::to_string(1 + ent.range(9));
        if (kind == GType::I64) text += "l";
        return text;
    }

    std::string float_literal_nonzero(GType::Kind kind) {
        std::string text = std::to_string(1 + ent.range(9));
        text += ".";
        text += std::to_string(ent.range(10));
        if (kind == GType::F32) text += "f";
        return text;
    }

    std::string literal(GType type, uint32_t depth) {
        switch (type.kind) {
            case GType::Bool: return ent.range(2) ? "true" : "false";
            case GType::I32:  return std::to_string(ent.range(100));
            case GType::I64:  return std::to_string(ent.range(1000)) + "l";
            case GType::F32:
            case GType::F64: {
                std::string text = std::to_string(ent.range(100));
                text += ".";
                text += std::to_string(ent.range(10));
                if (type.kind == GType::F32) text += "f";
                return text;
            }
            case GType::Str:  return "\"" + make_word() + "\"";
            case GType::EnumT: {
                const EnumInfo& enum_info = enums[type.index];
                return enum_info.name + "::" +
                       enum_info.variants[ent.range(static_cast<uint32_t>(enum_info.variants.size()))];
            }
            case GType::StructT:
                return struct_literal(type.index, depth);
        }
        return "0";
    }

    // Struct literals assign every field. Fields may be earlier structs of
    // the same module, so recursion strictly decreases the struct index and
    // terminates regardless of `depth`.
    std::string struct_literal(uint32_t struct_idx, uint32_t depth) {
        const StructInfo& struct_info = structs[struct_idx];
        std::string text = struct_info.name + " { ";
        for (size_t i = 0; i < struct_info.fields.size(); i++) {
            if (i > 0) text += ", ";
            text += struct_info.fields[i].name + " = " +
                    gen_expr(struct_info.fields[i].type, depth > 0 ? depth - 1 : 0);
        }
        text += " }";
        return text;
    }

    // ── Scope / callable lookups ──

    std::vector<size_t> vars_of(GType type) const {
        std::vector<size_t> result;
        for (size_t i = 0; i < scope.size(); i++) {
            if (scope[i].type == type) result.push_back(i);
        }
        return result;
    }

    std::vector<size_t> assignable_vars_of(GType type) const {
        std::vector<size_t> result;
        for (size_t i = 0; i < scope.size(); i++) {
            if (scope[i].assignable && scope[i].type == type) result.push_back(i);
        }
        return result;
    }

    bool func_visible(size_t func_idx) const {
        const FuncInfo& func = funcs[func_idx];
        if (func.receiver >= 0) return false;  // methods dispatch via a receiver var
        if (func.module_idx == cur_module) return true;
        if (!func.is_pub) return false;
        for (uint32_t imported : imports) {
            if (imported == func.module_idx) return true;
        }
        return false;
    }

    bool func_from_imported(size_t func_idx) const {
        for (size_t idx : from_imports) {
            if (idx == func_idx) return true;
        }
        return false;
    }

    std::string call_name(size_t func_idx) const {
        const FuncInfo& func = funcs[func_idx];
        if (func.module_idx == cur_module || func_from_imported(func_idx)) return func.name;
        return module_names[func.module_idx] + "." + func.name;
    }

    // Functions generated strictly before the current one (acyclic call graph),
    // visible from the current module, matching the wanted return type, and
    // affordable under the current loop nesting (dynamic-cost bound).
    std::vector<size_t> calls_returning(GType want) const {
        std::vector<size_t> result;
        for (size_t i = 0; i < cur_func && i < funcs.size(); i++) {
            if (funcs[i].has_ret && funcs[i].ret == want && func_visible(i) &&
                affordable(funcs[i])) {
                result.push_back(i);
            }
        }
        return result;
    }

    std::vector<size_t> void_calls() const {
        std::vector<size_t> result;
        for (size_t i = 0; i < cur_func && i < funcs.size(); i++) {
            if (!funcs[i].has_ret && func_visible(i) && affordable(funcs[i])) result.push_back(i);
        }
        return result;
    }

    // (receiver var index in scope, method func index) pairs with ret == want.
    std::vector<std::pair<size_t, size_t>> method_calls_returning(GType want) const {
        std::vector<std::pair<size_t, size_t>> result;
        for (size_t v = 0; v < scope.size(); v++) {
            if (scope[v].type.kind != GType::StructT) continue;
            for (size_t f = 0; f < cur_func && f < funcs.size(); f++) {
                if (funcs[f].receiver == static_cast<int>(scope[v].type.index) &&
                    funcs[f].has_ret && funcs[f].ret == want && affordable(funcs[f])) {
                    result.push_back({v, f});
                }
            }
        }
        return result;
    }

    // (struct var index in scope, field index) pairs with field type == want.
    std::vector<std::pair<size_t, size_t>> fields_of_type(GType want) const {
        std::vector<std::pair<size_t, size_t>> result;
        for (size_t v = 0; v < scope.size(); v++) {
            if (scope[v].type.kind != GType::StructT) continue;
            const StructInfo& struct_info = structs[scope[v].type.index];
            for (size_t f = 0; f < struct_info.fields.size(); f++) {
                if (struct_info.fields[f].type == want) result.push_back({v, f});
            }
        }
        return result;
    }

    std::vector<size_t> local_generic_fns() const {
        std::vector<size_t> result;
        for (size_t i = 0; i < generic_fns.size(); i++) {
            if (generic_fns[i].module_idx == cur_module) result.push_back(i);
        }
        return result;
    }

    std::vector<size_t> local_generic_structs() const {
        std::vector<size_t> result;
        for (size_t i = 0; i < generic_structs.size(); i++) {
            if (generic_structs[i].module_idx == cur_module) result.push_back(i);
        }
        return result;
    }

    std::string gen_call(size_t func_idx, uint32_t depth) {
        const FuncInfo& func = funcs[func_idx];
        fn_cost += clamped_mul(func.cost, cost_scale);
        std::string text = call_name(func_idx) + "(";
        for (size_t i = 0; i < func.params.size(); i++) {
            if (i > 0) text += ", ";
            text += gen_expr(func.params[i], depth > 0 ? depth - 1 : 0);
        }
        text += ")";
        return text;
    }

    // ── Expressions ─────────────────────────────────────────────────────────
    // Type-directed with a depth budget. The terminal choice (a var if one of
    // the wanted type is in scope, else a literal) is index 0, so a dry byte
    // buffer — where range() returns 0 — always produces terminal expressions.

    std::string terminal_expr(GType want, uint32_t depth) {
        auto vars = vars_of(want);
        if (!vars.empty() && ent.chance(70)) {
            return scope[vars[ent.range(static_cast<uint32_t>(vars.size()))]].name;
        }
        return literal(want, depth);
    }

    std::string gen_expr(GType want, uint32_t depth) {
        if (depth == 0) return terminal_expr(want, 0);

        // Option 0 is always the terminal expression.
        enum class Opt : uint8_t {
            Terminal, Binary, Unary, Cast, Call, Method, Field, GenericCall,
            Compare, Logical, Not, StrEq, Concat, FString,
        };
        std::vector<Opt> options = {Opt::Terminal, Opt::Terminal};

        bool have_calls = !calls_returning(want).empty();
        bool have_methods = !method_calls_returning(want).empty();
        bool have_fields = !fields_of_type(want).empty();
        bool have_generics = cfg.use_generics && !local_generic_fns().empty() &&
                             (want.is_prim() || want.kind == GType::StructT);

        if (want.is_numeric()) {
            options.push_back(Opt::Binary);
            options.push_back(Opt::Binary);
            options.push_back(Opt::Unary);
            if (want.kind != GType::Bool) options.push_back(Opt::Cast);
        } else if (want.kind == GType::Bool) {
            options.push_back(Opt::Compare);
            options.push_back(Opt::Compare);
            options.push_back(Opt::Logical);
            options.push_back(Opt::Not);
            options.push_back(Opt::StrEq);
        } else if (want.kind == GType::Str) {
            options.push_back(Opt::Concat);
            if (cfg.use_fstrings) options.push_back(Opt::FString);
        }
        if (have_calls) {
            options.push_back(Opt::Call);
            options.push_back(Opt::Call);
        }
        if (have_methods) options.push_back(Opt::Method);
        if (have_fields) options.push_back(Opt::Field);
        if (have_generics) options.push_back(Opt::GenericCall);

        switch (options[ent.range(static_cast<uint32_t>(options.size()))]) {
            case Opt::Terminal:
                return terminal_expr(want, depth);
            case Opt::Binary: {
                // Division only by a nonzero literal: no div-by-zero traps.
                uint32_t op = ent.range(4);
                if (op == 3) {
                    std::string rhs = (want.kind == GType::I32 || want.kind == GType::I64)
                                          ? int_literal_nonzero(want.kind)
                                          : float_literal_nonzero(want.kind);
                    return "(" + gen_expr(want, depth - 1) + " / " + rhs + ")";
                }
                const char* OPS[] = {" + ", " - ", " * "};
                return "(" + gen_expr(want, depth - 1) + OPS[op] + gen_expr(want, depth - 1) + ")";
            }
            case Opt::Unary:
                return "(-" + gen_expr(want, depth - 1) + ")";
            case Opt::Cast: {
                // int<->int and int->float only; float->int conversion of an
                // out-of-range value is UB territory we deliberately avoid.
                GType source;
                switch (want.kind) {
                    case GType::I32: source = GType{GType::I64, 0}; break;
                    case GType::I64: source = GType{GType::I32, 0}; break;
                    default:         source = GType{ent.range(2) ? GType::I32 : GType::I64, 0}; break;
                }
                return type_name(want) + "(" + gen_expr(source, depth - 1) + ")";
            }
            case Opt::Call: {
                auto candidates = calls_returning(want);
                return gen_call(candidates[ent.range(static_cast<uint32_t>(candidates.size()))], depth);
            }
            case Opt::Method: {
                auto candidates = method_calls_returning(want);
                auto [var_idx, func_idx] = candidates[ent.range(static_cast<uint32_t>(candidates.size()))];
                const FuncInfo& method = funcs[func_idx];
                fn_cost += clamped_mul(method.cost, cost_scale);
                std::string text = scope[var_idx].name + "." + method.name + "(";
                for (size_t i = 0; i < method.params.size(); i++) {
                    if (i > 0) text += ", ";
                    text += gen_expr(method.params[i], depth - 1);
                }
                text += ")";
                return text;
            }
            case Opt::Field: {
                auto candidates = fields_of_type(want);
                auto [var_idx, field_idx] = candidates[ent.range(static_cast<uint32_t>(candidates.size()))];
                return scope[var_idx].name + "." + structs[scope[var_idx].type.index].fields[field_idx].name;
            }
            case Opt::GenericCall: {
                auto candidates = local_generic_fns();
                const GenericFnInfo& generic = generic_fns[candidates[ent.range(static_cast<uint32_t>(candidates.size()))]];
                // Sometimes explicit type args (exercises the parser's
                // angle-bracket trial-parse), sometimes inferred.
                std::string callee = generic.name;
                if (ent.chance(40)) callee += "<" + type_name(want) + ">";
                return callee + "(" + gen_expr(want, depth - 1) + ")";
            }
            case Opt::Compare: {
                GType operand{ent.range(2) ? GType::I32 : GType::I64, 0};
                if (ent.chance(25)) operand = GType{ent.range(2) ? GType::F32 : GType::F64, 0};
                static const char* CMPS[] = {" == ", " != ", " < ", " <= ", " > ", " >= "};
                return "(" + gen_expr(operand, depth - 1) + CMPS[ent.range(6)] +
                       gen_expr(operand, depth - 1) + ")";
            }
            case Opt::Logical: {
                const char* op = ent.range(2) ? " && " : " || ";
                return "(" + gen_expr(GType{GType::Bool, 0}, depth - 1) + op +
                       gen_expr(GType{GType::Bool, 0}, depth - 1) + ")";
            }
            case Opt::Not:
                return "(!" + gen_expr(GType{GType::Bool, 0}, depth - 1) + ")";
            case Opt::StrEq:
                return "str_eq(" + gen_expr(GType{GType::Str, 0}, depth - 1) + ", " +
                       gen_expr(GType{GType::Str, 0}, depth - 1) + ")";
            case Opt::Concat:
                return "str_concat(" + gen_expr(GType{GType::Str, 0}, depth - 1) + ", " +
                       gen_expr(GType{GType::Str, 0}, depth - 1) + ")";
            case Opt::FString: {
                // Interpolants are terminal primitive expressions (vars or
                // literals) — all covered by the builtin Printable trait.
                std::string text = "f\"" + make_word();
                uint32_t interp_count = 1 + ent.range(2);
                for (uint32_t i = 0; i < interp_count; i++) {
                    GType prim = random_prim_type();
                    std::string interp;
                    if (prim.kind == GType::Str) {
                        // A string literal here would nest quotes inside the
                        // f-string; only interpolate string *variables*.
                        auto string_vars = vars_of(prim);
                        if (string_vars.empty()) prim = GType{GType::I32, 0};
                    }
                    if (prim.kind == GType::Str) {
                        auto string_vars = vars_of(prim);
                        interp = scope[string_vars[ent.range(static_cast<uint32_t>(string_vars.size()))]].name;
                    } else {
                        interp = terminal_expr(prim, 0);
                    }
                    text += " {" + interp + "} " + make_word();
                }
                text += "\"";
                return text;
            }
        }
        return literal(want, depth);
    }

    // ── Statements ──────────────────────────────────────────────────────────

    void gen_block_stmts(uint32_t stmt_count) {
        size_t scope_mark = scope.size();
        block_depth++;
        for (uint32_t i = 0; i < stmt_count && stmt_budget > 0; i++) {
            stmt_budget--;
            gen_stmt();
        }
        block_depth--;
        scope.resize(scope_mark);
    }

    void gen_var_decl() {
        GType type = random_var_type();
        std::string name = fresh_name(false);
        line("var " + name + ": " + type_name(type) + " = " + gen_expr(type, cfg.max_expr_depth) + ";");
        scope.push_back({name, type, true});
    }

    void gen_stmt() {
        fn_cost += cost_scale;  // every statement costs ~1 per execution
        enum class Opt : uint8_t {
            VarDecl, Assign, FieldAssign, If, For, While, When, CallStmt,
            EarlyReturn, GenericBox,
        };
        // Index 0 (VarDecl) is the terminal statement for dry entropy.
        std::vector<Opt> options = {Opt::VarDecl, Opt::VarDecl};

        bool can_nest = block_depth < cfg.max_block_depth;
        bool have_assignable = false;
        for (const VarInfo& var : scope) {
            if (var.assignable) { have_assignable = true; break; }
        }
        bool have_enum_var = false;
        for (const VarInfo& var : scope) {
            if (var.type.kind == GType::EnumT) { have_enum_var = true; break; }
        }

        if (have_assignable) {
            options.push_back(Opt::Assign);
            options.push_back(Opt::Assign);
        }
        if (!fields_of_assignable_structs().empty()) options.push_back(Opt::FieldAssign);
        if (can_nest) {
            options.push_back(Opt::If);
            options.push_back(Opt::If);
            options.push_back(Opt::For);
            options.push_back(Opt::While);
            if (have_enum_var) options.push_back(Opt::When);
            if (cur_has_ret) options.push_back(Opt::EarlyReturn);
        }
        if (any_calls_exist()) options.push_back(Opt::CallStmt);
        if (cfg.use_generics && !local_generic_structs().empty()) options.push_back(Opt::GenericBox);

        switch (options[ent.range(static_cast<uint32_t>(options.size()))]) {
            case Opt::VarDecl:
                gen_var_decl();
                break;
            case Opt::Assign: {
                std::vector<size_t> candidates;
                for (size_t i = 0; i < scope.size(); i++) {
                    if (scope[i].assignable) candidates.push_back(i);
                }
                const VarInfo& var = scope[candidates[ent.range(static_cast<uint32_t>(candidates.size()))]];
                line(var.name + " = " + gen_expr(var.type, cfg.max_expr_depth) + ";");
                break;
            }
            case Opt::FieldAssign: {
                auto candidates = fields_of_assignable_structs();
                auto [var_idx, field_idx] = candidates[ent.range(static_cast<uint32_t>(candidates.size()))];
                const FieldInfo& field = structs[scope[var_idx].type.index].fields[field_idx];
                line(scope[var_idx].name + "." + field.name + " = " +
                     gen_expr(field.type, cfg.max_expr_depth) + ";");
                break;
            }
            case Opt::If: {
                bool has_else = ent.chance(40);
                open("if (" + gen_expr(GType{GType::Bool, 0}, cfg.max_expr_depth) + ")");
                gen_block_stmts(1 + ent.range(cfg.max_stmts_per_block));
                if (has_else) {
                    indent--;
                    line("} else {");
                    indent++;
                    gen_block_stmts(1 + ent.range(cfg.max_stmts_per_block));
                }
                close();
                break;
            }
            case Opt::For: {
                std::string counter = fresh_name(false);
                uint32_t trip_count = 1 + ent.range(8);
                open("for (var " + counter + ": i32 = 0; " + counter + " < " +
                     std::to_string(trip_count) + "; " + counter + " = " + counter + " + 1)");
                scope.push_back({counter, GType{GType::I32, 0}, false});
                uint64_t saved_scale = cost_scale;
                cost_scale = clamped_mul(cost_scale, trip_count);
                gen_block_stmts(1 + ent.range(cfg.max_stmts_per_block));
                cost_scale = saved_scale;
                scope.pop_back();
                close();
                break;
            }
            case Opt::While: {
                // Counter pattern with the decrement emitted unconditionally at
                // the end of the body; the counter is not assignable by
                // generated statements, so the loop always terminates.
                std::string counter = fresh_name(false);
                uint32_t trip_count = 1 + ent.range(8);
                line("var " + counter + ": i32 = " + std::to_string(trip_count) + ";");
                scope.push_back({counter, GType{GType::I32, 0}, false});
                open("while (" + counter + " > 0)");
                uint64_t saved_scale = cost_scale;
                cost_scale = clamped_mul(cost_scale, trip_count);
                gen_block_stmts(1 + ent.range(cfg.max_stmts_per_block));
                cost_scale = saved_scale;
                line(counter + " = " + counter + " - 1;");
                close();
                // The counter stays in scope after the loop (readable, == 0).
                break;
            }
            case Opt::When: {
                std::vector<size_t> enum_vars;
                for (size_t i = 0; i < scope.size(); i++) {
                    if (scope[i].type.kind == GType::EnumT) enum_vars.push_back(i);
                }
                const VarInfo& discriminant =
                    scope[enum_vars[ent.range(static_cast<uint32_t>(enum_vars.size()))]];
                const EnumInfo& enum_info = enums[discriminant.type.index];
                // Either exhaustive (all variants, no else) or a strict prefix
                // of the variants plus an else — both forms are valid.
                bool exhaustive = ent.chance(50);
                size_t case_count = exhaustive
                    ? enum_info.variants.size()
                    : 1 + ent.range(static_cast<uint32_t>(enum_info.variants.size() - 1));
                open("when " + discriminant.name);
                for (size_t i = 0; i < case_count; i++) {
                    open("case " + enum_info.variants[i] + ":");
                    gen_block_stmts(1 + ent.range(2));
                    close();
                }
                if (!exhaustive) {
                    open("else:");
                    gen_block_stmts(1 + ent.range(2));
                    close();
                }
                close();
                break;
            }
            case Opt::CallStmt: {
                auto candidates = void_calls();
                if (!candidates.empty() && ent.chance(60)) {
                    line(gen_call(candidates[ent.range(static_cast<uint32_t>(candidates.size()))],
                                  cfg.max_expr_depth) + ";");
                } else {
                    // Capture some call's result into a fresh var instead.
                    GType type = random_prim_type();
                    auto returning = calls_returning(type);
                    if (returning.empty()) {
                        gen_var_decl();
                    } else {
                        std::string name = fresh_name(false);
                        line("var " + name + ": " + type_name(type) + " = " +
                             gen_call(returning[ent.range(static_cast<uint32_t>(returning.size()))],
                                      cfg.max_expr_depth) + ";");
                        scope.push_back({name, type, true});
                    }
                }
                break;
            }
            case Opt::EarlyReturn:
                // Then-branch-only return: statements after this stay reachable.
                open("if (" + gen_expr(GType{GType::Bool, 0}, cfg.max_expr_depth) + ")");
                line("return " + gen_expr(cur_ret, cfg.max_expr_depth) + ";");
                close();
                break;
            case Opt::GenericBox: {
                auto candidates = local_generic_structs();
                const GenericStructInfo& generic =
                    generic_structs[candidates[ent.range(static_cast<uint32_t>(candidates.size()))]];
                GType inner = random_prim_type();
                std::string box_name = fresh_name(false);
                std::string value_name = fresh_name(false);
                // T is inferred from the field value; reading it back keeps
                // the instantiation alive in downstream expressions.
                line("var " + box_name + " = " + generic.name + " { value = " +
                     gen_expr(inner, 1) + " };");
                line("var " + value_name + ": " + type_name(inner) + " = " + box_name + ".value;");
                scope.push_back({value_name, inner, true});
                break;
            }
        }
    }

    std::vector<std::pair<size_t, size_t>> fields_of_assignable_structs() const {
        std::vector<std::pair<size_t, size_t>> result;
        for (size_t v = 0; v < scope.size(); v++) {
            if (!scope[v].assignable || scope[v].type.kind != GType::StructT) continue;
            const StructInfo& struct_info = structs[scope[v].type.index];
            for (size_t f = 0; f < struct_info.fields.size(); f++) {
                result.push_back({v, f});
            }
        }
        return result;
    }

    bool any_calls_exist() const {
        for (size_t i = 0; i < cur_func && i < funcs.size(); i++) {
            if (func_visible(i)) return true;
        }
        return false;
    }

    // ── Declarations ────────────────────────────────────────────────────────

    void gen_enum() {
        EnumInfo enum_info;
        enum_info.name = fresh_name(true);
        enum_info.module_idx = cur_module;
        uint32_t variant_count = 2 + ent.range(4);
        std::string body;
        for (uint32_t i = 0; i < variant_count; i++) {
            std::string variant = fresh_name(true);
            if (i > 0) body += ", ";
            body += variant;
            enum_info.variants.push_back(variant);
        }
        line("enum " + enum_info.name + " { " + body + " }");
        line("");
        enums.push_back(std::move(enum_info));
        stats.enums++;
    }

    void gen_struct() {
        StructInfo struct_info;
        struct_info.name = fresh_name(true);
        struct_info.module_idx = cur_module;
        uint32_t field_count = 1 + ent.range(4);
        auto earlier_structs = local_structs();
        auto earlier_enums = local_enums();
        for (uint32_t i = 0; i < field_count; i++) {
            GType field_type = random_prim_type();
            if (!earlier_structs.empty() && ent.chance(20)) {
                field_type = GType{GType::StructT,
                                   earlier_structs[ent.range(static_cast<uint32_t>(earlier_structs.size()))]};
            } else if (!earlier_enums.empty() && ent.chance(15)) {
                field_type = GType{GType::EnumT,
                                   earlier_enums[ent.range(static_cast<uint32_t>(earlier_enums.size()))]};
            }
            struct_info.fields.push_back({fresh_name(false), field_type});
        }
        open("struct " + struct_info.name);
        for (const FieldInfo& field : struct_info.fields) {
            line(field.name + ": " + type_name(field.type) + ";");
        }
        close();
        line("");
        structs.push_back(std::move(struct_info));
        stats.structs++;
    }

    void gen_signature(FuncInfo& func, bool force_pub_prim) {
        uint32_t param_count = ent.range(cfg.max_params + 1);
        bool all_prim = true;
        for (uint32_t i = 0; i < param_count; i++) {
            GType param_type = force_pub_prim ? random_prim_type() : random_var_type();
            if (!param_type.is_prim()) all_prim = false;
            func.params.push_back(param_type);
        }
        if (force_pub_prim) {
            func.has_ret = true;
            func.ret = GType{GType::I32, 0};
        } else {
            func.has_ret = ent.chance(80);
            if (func.has_ret) {
                func.ret = random_var_type();
                if (!func.ret.is_prim()) all_prim = false;
            }
        }
        // pub requires a primitive-only signature: module-local struct/enum
        // types cannot cross the module boundary in v1.
        func.is_pub = func.receiver < 0 && all_prim && (force_pub_prim || ent.chance(50));
    }

    void gen_function_body(const FuncInfo& func, const std::vector<std::string>& param_names) {
        scope.clear();
        block_depth = 0;
        stmt_budget = cfg.max_stmts_per_function;
        fn_cost = 0;
        cost_scale = 1;
        cur_has_ret = func.has_ret;
        cur_ret = func.ret;
        for (size_t i = 0; i < func.params.size(); i++) {
            // Params are conservatively non-assignable (value-semantics
            // reassignment is not a property we need to test yet).
            scope.push_back({param_names[i], func.params[i], false});
        }
        if (func.receiver >= 0) {
            for (const FieldInfo& field : structs[func.receiver].fields) {
                scope.push_back({"self." + field.name, field.type, true});
            }
        }
        gen_block_stmts(1 + ent.range(cfg.max_stmts_per_block));
        if (func.has_ret) {
            line("return " + gen_expr(func.ret, cfg.max_expr_depth) + ";");
        }
    }

    void gen_function(int receiver, bool force_pub_prim) {
        FuncInfo func;
        func.name = fresh_name(false);
        func.module_idx = cur_module;
        func.receiver = receiver;
        gen_signature(func, force_pub_prim);

        std::vector<std::string> param_names;
        std::string head = func.is_pub ? "pub fun " : "fun ";
        if (receiver >= 0) head += structs[receiver].name + ".";
        head += func.name + "(";
        for (size_t i = 0; i < func.params.size(); i++) {
            param_names.push_back(fresh_name(false));
            if (i > 0) head += ", ";
            head += param_names[i] + ": " + type_name(func.params[i]);
        }
        head += ")";
        if (func.has_ret) head += ": " + type_name(func.ret);

        // Register before generating the body; calls_returning() only sees
        // strictly earlier functions, so the function cannot call itself.
        cur_func = funcs.size();
        funcs.push_back(func);
        open(head);
        gen_function_body(func, param_names);
        close();
        line("");
        // Record the body's accumulated dynamic cost so later callers can
        // bound call-in-loop composition.
        funcs[cur_func].cost = fn_cost + 1;
        stats.functions++;
    }

    void gen_generic_fn() {
        GenericFnInfo generic;
        generic.name = fresh_name(false);
        generic.module_idx = cur_module;
        open("fun " + generic.name + "<T>(v: T): T");
        line("return v;");
        close();
        line("");
        generic_fns.push_back(std::move(generic));
        stats.functions++;
    }

    void gen_generic_struct() {
        GenericStructInfo generic;
        generic.name = fresh_name(true);
        generic.module_idx = cur_module;
        open("struct " + generic.name + "<T>");
        line("value: T;");
        close();
        line("");
        generic_structs.push_back(std::move(generic));
        stats.structs++;
    }

    // ── main() ──────────────────────────────────────────────────────────────
    // Accumulates an i32 checksum from cross-module pub calls (guaranteeing
    // every imported module is exercised, not just discovered) plus ordinary
    // generated statements, and returns it. With allow_print, also prints it —
    // the observable output for a future VM-vs-C differential oracle.

    void gen_checksum_stmt(const std::string& acc, size_t func_idx) {
        const FuncInfo& func = funcs[func_idx];
        std::string call = gen_call(func_idx, 1);
        if (!func.has_ret) {
            line(call + ";");
            return;
        }
        switch (func.ret.kind) {
            case GType::I32:
                line(acc + " = (" + acc + " + " + call + ");");
                break;
            case GType::I64:
                line(acc + " = (" + acc + " + i32(" + call + "));");
                break;
            case GType::F32:
            case GType::F64:
                open("if (" + call + " < 1000000.0" +
                     (func.ret.kind == GType::F32 ? std::string("f") : std::string("")) + ")");
                line(acc + " = (" + acc + " + 1);");
                close();
                break;
            case GType::Bool:
                open("if (" + call + ")");
                line(acc + " = (" + acc + " + 1);");
                close();
                break;
            case GType::Str:
                line(acc + " = (" + acc + " + str_len(" + call + "));");
                break;
            default:
                line(call + ";");
                break;
        }
    }

    void gen_main_fn() {
        cur_func = funcs.size();  // main can call everything generated so far
        cur_has_ret = true;
        cur_ret = GType{GType::I32, 0};
        scope.clear();
        block_depth = 0;
        stmt_budget = cfg.max_stmts_per_function;
        fn_cost = 0;
        cost_scale = 1;

        std::string acc = fresh_name(false);
        open("fun main(): i32");
        line("var " + acc + ": i32 = 0;");
        scope.push_back({acc, GType{GType::I32, 0}, true});

        // Checksum calls into imported modules' pub functions. Capped so a
        // large corpus's main (which imports every otherwise-unimported
        // module) doesn't itself become a register-pressure monster — the
        // imports alone already pull every module into the compile.
        uint32_t checksum_budget = 30;
        for (uint32_t imported : imports) {
            if (checksum_budget == 0) break;
            std::vector<size_t> pub_funcs;
            for (size_t i = 0; i < funcs.size(); i++) {
                if (funcs[i].module_idx == imported && funcs[i].is_pub && affordable(funcs[i])) {
                    pub_funcs.push_back(i);
                }
            }
            if (pub_funcs.empty()) continue;
            uint32_t call_count = 1 + ent.range(2);
            for (uint32_t c = 0; c < call_count && checksum_budget > 0; c++) {
                gen_checksum_stmt(acc, pub_funcs[ent.range(static_cast<uint32_t>(pub_funcs.size()))]);
                checksum_budget--;
            }
        }

        gen_block_stmts(1 + ent.range(cfg.max_stmts_per_block));
        line(acc + " = (" + acc + " + " + gen_expr(GType{GType::I32, 0}, cfg.max_expr_depth) + ");");
        if (cfg.allow_print) {
            // Benchmark corpora: print the checksum (observable output for a
            // future VM-vs-C differential) and exit 0 so the CLI's exit code
            // stays clean. Fuzz/test mode returns the checksum instead — the
            // harnesses observe it through vm_get_result.
            line("print(f\"checksum: {" + acc + "}\");");
            line("return 0;");
        } else {
            line("return " + acc + ";");
        }
        close();
        stats.functions++;
    }

    // ── Modules ─────────────────────────────────────────────────────────────

    void emit_module_body(bool is_main) {
        line("// Generated by the Roxy structural generator (tests/fuzz/gen).");
        line("");

        for (uint32_t imported : imports) {
            line("import " + module_names[imported] + ";");
        }
        // Occasionally also pull one pub function in via `from ... import`
        // (globally unique names make this collision-free by construction).
        from_imports.clear();
        for (uint32_t imported : imports) {
            if (!ent.chance(25)) continue;
            std::vector<size_t> pub_funcs;
            for (size_t i = 0; i < funcs.size(); i++) {
                if (funcs[i].module_idx == imported && funcs[i].is_pub) pub_funcs.push_back(i);
            }
            if (pub_funcs.empty()) continue;
            size_t func_idx = pub_funcs[ent.range(static_cast<uint32_t>(pub_funcs.size()))];
            line("from " + module_names[imported] + " import " + funcs[func_idx].name + ";");
            from_imports.push_back(func_idx);
        }
        if (!imports.empty()) line("");

        uint32_t enum_count = ent.range(cfg.max_enums_per_module + 1);
        for (uint32_t i = 0; i < enum_count; i++) gen_enum();

        uint32_t struct_count = ent.range(cfg.max_structs_per_module + 1);
        for (uint32_t i = 0; i < struct_count; i++) gen_struct();

        if (cfg.use_methods) {
            auto own_structs = local_structs();
            for (uint32_t struct_idx : own_structs) {
                uint32_t method_count = ent.range(cfg.max_methods_per_struct + 1);
                for (uint32_t i = 0; i < method_count; i++) {
                    gen_function(static_cast<int>(struct_idx), false);
                }
            }
        }

        if (cfg.use_generics) {
            if (ent.chance(50)) gen_generic_fn();
            if (ent.chance(35)) gen_generic_struct();
        }

        uint32_t func_count = cfg.min_funcs_per_module +
                              ent.range(cfg.max_funcs_per_module - cfg.min_funcs_per_module + 1);
        for (uint32_t i = 0; i < func_count; i++) {
            // Every non-main module leads with a pub i32 function so main's
            // checksum always has something concrete to call.
            bool force_pub_prim = !is_main && i == 0;
            gen_function(-1, force_pub_prim);
        }

        if (is_main) gen_main_fn();
    }

    GeneratedProgram run() {
        uint32_t module_count = cfg.num_modules > 0 ? cfg.num_modules : 1;
        for (uint32_t i = 0; i + 1 < module_count; i++) {
            module_names.push_back("m" + std::to_string(i) + "_" + make_word());
        }
        module_names.push_back("main");

        GeneratedProgram program;
        std::vector<bool> imported_by_someone(module_count, false);

        for (uint32_t i = 0; i < module_count; i++) {
            cur_module = i;
            bool is_main = (i + 1 == module_count);
            imports.clear();

            if (cfg.use_cross_module && i > 0) {
                if (is_main) {
                    // main imports every module nothing else imports (so the
                    // CLI's import-driven discovery reaches all files), plus a
                    // few extras.
                    for (uint32_t j = 0; j + 1 < module_count; j++) {
                        if (!imported_by_someone[j] || ent.chance(10)) imports.push_back(j);
                    }
                } else {
                    // Import 1-3 earlier modules, biased toward low indices —
                    // early modules accumulate high fan-in like real "core"
                    // modules do.
                    uint32_t import_count = 1 + ent.range(3);
                    for (uint32_t c = 0; c < import_count; c++) {
                        uint32_t candidate = ent.range(ent.range(i) + 1);
                        bool duplicate = false;
                        for (uint32_t existing : imports) {
                            if (existing == candidate) duplicate = true;
                        }
                        if (!duplicate) imports.push_back(candidate);
                    }
                }
                for (uint32_t imported : imports) imported_by_someone[imported] = true;
            }

            out.clear();
            indent = 0;
            emit_module_body(is_main);
            program.modules.push_back({module_names[i], out});
            stats.modules++;
        }

        program.stats = stats;
        return program;
    }
};

} // namespace

GeneratedProgram generate_program(Entropy& entropy, const GenConfig& config) {
    Generator generator(entropy, config);
    return generator.run();
}

} // namespace rx::gen
