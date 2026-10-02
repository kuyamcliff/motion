// Source-to-source rewrite of arithmetic operators into vector-aware helper calls.
// Supports the JavaScript subset typically used in expressions/scripts; anything else makes the
// rewriter bail out (callers then run the source unmodified).
#include <stdexcept>

#include "mf/expressions.hpp"

namespace mf {

namespace {

enum class TK { Num, Str, Ident, Punct, End };
struct Tok {
    TK k;
    std::string s;
    int line = 1;
};

struct Bail : std::runtime_error {
    Bail() : std::runtime_error("unsupported") {}
};

std::vector<Tok> tokenize(const std::string& src) {
    std::vector<Tok> out;
    size_t i = 0, n = src.size();
    int line = 1;
    auto lineAt = [&](size_t upto) {
        // count newlines consumed so far lazily
        return line;
    };
    (void)lineAt;
    static const char* puncts[] = {">>>=", "===", "!==", "**=", "<<=", ">>=", ">>>", "...", "=>", "==", "!=", "<=", ">=", "&&", "||", "??", "++", "--", "+=", "-=",
                                   "*=", "/=", "%=", "&=", "|=", "^=", "**", "<<", ">>", "?.", "+", "-", "*", "/", "%", "=", "<", ">", "!", "~", "&",
                                   "|", "^", "?", ":", ";", ",", ".", "(", ")", "[", "]", "{", "}"};
    while (i < n) {
        char c = src[i];
        if (std::isspace((unsigned char)c)) { if (c == '\n') ++line; ++i; continue; }
        if (c == '/' && i + 1 < n && src[i + 1] == '/') { while (i < n && src[i] != '\n') ++i; continue; }
        if (c == '/' && i + 1 < n && src[i + 1] == '*') {
            size_t e = src.find("*/", i + 2);
            if (e == std::string::npos) throw Bail();
            line += (int)std::count(src.begin() + i, src.begin() + e, '\n');
            i = e + 2;
            continue;
        }
        if (std::isdigit((unsigned char)c) || (c == '.' && i + 1 < n && std::isdigit((unsigned char)src[i + 1]))) {
            size_t s = i;
            if (c == '0' && i + 1 < n && (src[i + 1] == 'x' || src[i + 1] == 'X')) {
                i += 2;
                while (i < n && std::isxdigit((unsigned char)src[i])) ++i;
            } else {
                while (i < n && (std::isdigit((unsigned char)src[i]) || src[i] == '.')) ++i;
                if (i < n && (src[i] == 'e' || src[i] == 'E')) {
                    ++i;
                    if (i < n && (src[i] == '+' || src[i] == '-')) ++i;
                    while (i < n && std::isdigit((unsigned char)src[i])) ++i;
                }
            }
            out.push_back({TK::Num, src.substr(s, i - s), line});
            continue;
        }
        if (std::isalpha((unsigned char)c) || c == '_' || c == '$') {
            size_t s = i;
            while (i < n && (std::isalnum((unsigned char)src[i]) || src[i] == '_' || src[i] == '$')) ++i;
            out.push_back({TK::Ident, src.substr(s, i - s), line});
            continue;
        }
        if (c == '"' || c == '\'') {
            size_t s = i++;
            while (i < n && src[i] != c) {
                if (src[i] == '\\') ++i;
                if (i < n && src[i] == '\n') throw Bail();
                ++i;
            }
            if (i >= n) throw Bail();
            ++i;
            out.push_back({TK::Str, src.substr(s, i - s), line});
            continue;
        }
        if (c == '`') {
            size_t s = i++;
            while (i < n && src[i] != '`') {
                if (src[i] == '\\') ++i;
                else if (src[i] == '$' && i + 1 < n && src[i + 1] == '{') throw Bail();  // template substitutions unsupported
                ++i;
            }
            if (i >= n) throw Bail();
            ++i;
            out.push_back({TK::Str, src.substr(s, i - s), line});
            continue;
        }
        bool matched = false;
        for (const char* p : puncts) {
            size_t L = std::strlen(p);
            if (src.compare(i, L, p) == 0) {
                // Regex literals are not supported (ambiguous with division).
                out.push_back({TK::Punct, p, line});
                i += L;
                matched = true;
                break;
            }
        }
        if (!matched) throw Bail();
    }
    out.push_back({TK::End, "", line});
    return out;
}

class Parser {
   public:
    explicit Parser(std::vector<Tok> t) : t_(std::move(t)) {}

    std::string program() {
        std::string out;
        while (!at(TK::End)) out += statement();
        return out;
    }

   private:
    std::vector<Tok> t_;
    size_t p_ = 0;
    int outLine_ = 1;

    // Emit newlines so statements keep their original line numbers in error messages.
    std::string catchUp() {
        std::string nl;
        while (outLine_ < cur().line) { nl += "\n"; ++outLine_; }
        return nl;
    }

    const Tok& cur() const { return t_[p_]; }
    bool at(TK k) const { return cur().k == k; }
    bool isP(const char* s) const { return cur().k == TK::Punct && cur().s == s; }
    bool isId(const char* s) const { return cur().k == TK::Ident && cur().s == s; }
    void expectP(const char* s) {
        if (!isP(s)) throw Bail();
        ++p_;
    }
    std::string take() { return t_[p_++].s; }

    std::string block() {
        expectP("{");
        std::string out = "{";
        while (!isP("}")) {
            if (at(TK::End)) throw Bail();
            out += statement();
        }
        ++p_;
        return out + "}";
    }

    std::string statement() {
        std::string pre = catchUp();
        return pre + statementBody();
    }

    std::string statementBody() {
        if (isP("{")) return block();
        if (isP(";")) { ++p_; return ";"; }
        if (isId("var") || isId("let") || isId("const")) {
            std::string kw = take();
            std::string out = kw + " " + declarators();
            if (isP(";")) ++p_;
            return out + ";";
        }
        if (isId("if")) {
            ++p_;
            expectP("(");
            std::string c = expression();
            expectP(")");
            std::string out = "if(" + c + ")" + statement();
            if (isId("else")) { ++p_; out += " else " + statement(); }
            return out;
        }
        if (isId("while")) {
            ++p_;
            expectP("(");
            std::string c = expression();
            expectP(")");
            return "while(" + c + ")" + statement();
        }
        if (isId("for")) {
            ++p_;
            expectP("(");
            std::string init;
            if (isId("var") || isId("let") || isId("const")) {
                std::string kw = take();
                // for (const x of arr) / for (let k in obj)
                if (cur().k == TK::Ident && (t_[p_ + 1].s == "of" || t_[p_ + 1].s == "in")) {
                    std::string name = take();
                    std::string ofin = take();
                    std::string e = expression();
                    expectP(")");
                    return "for(" + kw + " " + name + " " + ofin + " " + e + ")" + statement();
                }
                init = kw + " " + declarators();
            } else if (!isP(";")) {
                init = expression();
            }
            expectP(";");
            std::string cond = isP(";") ? "" : expression();
            expectP(";");
            std::string step = isP(")") ? "" : expression();
            expectP(")");
            return "for(" + init + ";" + cond + ";" + step + ")" + statement();
        }
        if (isId("return")) {
            ++p_;
            std::string e = (isP(";") || isP("}") || at(TK::End)) ? "" : expression();
            if (isP(";")) ++p_;
            return "return " + e + ";";
        }
        if (isId("break") || isId("continue")) {
            std::string k = take();
            if (isP(";")) ++p_;
            return k + ";";
        }
        if (isId("function")) {
            return functionExpr() + ";";
        }
        if (isId("throw")) {
            ++p_;
            std::string e = expression();
            if (isP(";")) ++p_;
            return "throw " + e + ";";
        }
        if (isId("try")) {
            ++p_;
            std::string out = "try" + block();
            if (isId("catch")) {
                ++p_;
                out += "catch";
                if (isP("(")) { ++p_; out += "(" + take() + ")"; expectP(")"); }
                out += block();
            }
            if (isId("finally")) { ++p_; out += "finally" + block(); }
            return out;
        }
        if (isId("switch") || isId("class") || isId("import") || isId("export") || isId("with") || isId("do")) throw Bail();
        std::string e = expression();
        if (isP(";")) ++p_;
        return e + ";";
    }

    std::string declarators() {
        std::string out;
        for (;;) {
            if (cur().k != TK::Ident) throw Bail();  // destructuring unsupported
            out += take();
            if (isP("=")) { ++p_; out += "=" + assignment(); }
            if (isP(",")) { ++p_; out += ","; continue; }
            break;
        }
        return out;
    }

    std::string functionExpr() {
        expectP_id("function");
        std::string name;
        if (cur().k == TK::Ident) name = take();
        expectP("(");
        std::string params;
        while (!isP(")")) {
            if (cur().k != TK::Ident) throw Bail();
            params += take();
            if (isP("=")) { ++p_; params += "=" + assignment(); }
            if (isP(",")) { ++p_; params += ","; }
        }
        ++p_;
        return "function " + name + "(" + params + ")" + block();
    }
    void expectP_id(const char* s) {
        if (!isId(s)) throw Bail();
        ++p_;
    }

    std::string expression() {
        std::string e = assignment();
        while (isP(",")) { ++p_; e += "," + assignment(); }
        return e;
    }

    std::string assignment() {
        // Arrow function: ident => ... or (a,b) => ...
        if (cur().k == TK::Ident && t_[p_ + 1].k == TK::Punct && t_[p_ + 1].s == "=>") {
            std::string param = take();
            ++p_;
            return "(" + param + ")=>" + arrowBody();
        }
        if (isP("(")) {
            size_t save = p_;
            // Look ahead for "(...) =>"
            int depth = 0;
            size_t q = p_;
            for (; t_[q].k != TK::End; ++q) {
                if (t_[q].k == TK::Punct && t_[q].s == "(") ++depth;
                if (t_[q].k == TK::Punct && t_[q].s == ")") { if (--depth == 0) break; }
            }
            if (t_[q].k != TK::End && t_[q + 1].k == TK::Punct && t_[q + 1].s == "=>") {
                std::string params;
                ++p_;
                while (!isP(")")) {
                    if (cur().k != TK::Ident) throw Bail();
                    params += take();
                    if (isP(",")) { ++p_; params += ","; }
                }
                ++p_;
                expectP("=>");
                return "(" + params + ")=>" + arrowBody();
            }
            p_ = save;
        }
        size_t start = p_;
        std::string lhs = conditional();
        if (cur().k == TK::Punct) {
            const std::string& op = cur().s;
            if (op == "=") { ++p_; return lhs + "=" + assignment(); }
            static const std::map<std::string, std::string> compound = {{"+=", "__add"}, {"-=", "__sub"}, {"*=", "__mul"}, {"/=", "__div"}};
            auto it = compound.find(op);
            if (it != compound.end()) {
                ++p_;
                std::string rhs = assignment();
                return lhs + "=" + it->second + "(" + lhs + "," + rhs + ")";
            }
            if (op == "%=" || op == "&=" || op == "|=" || op == "^=" || op == "<<=" || op == ">>=" || op == "**=" || op == ">>>=") {
                ++p_;
                return lhs + op + assignment();
            }
        }
        (void)start;
        return lhs;
    }

    std::string arrowBody() {
        if (isP("{")) return block();
        return "(" + assignment() + ")";
    }

    std::string conditional() {
        std::string c = binary(0);
        if (isP("?")) {
            ++p_;
            std::string a = assignment();
            expectP(":");
            std::string b = assignment();
            return c + "?" + a + ":" + b;
        }
        return c;
    }

    static int prec(const std::string& op) {
        if (op == "??") return 1;
        if (op == "||") return 2;
        if (op == "&&") return 3;
        if (op == "|") return 4;
        if (op == "^") return 5;
        if (op == "&") return 6;
        if (op == "==" || op == "!=" || op == "===" || op == "!==") return 7;
        if (op == "<" || op == ">" || op == "<=" || op == ">=" || op == "instanceof" || op == "in") return 8;
        if (op == "<<" || op == ">>" || op == ">>>") return 9;
        if (op == "+" || op == "-") return 10;
        if (op == "*" || op == "/" || op == "%") return 11;
        if (op == "**") return 12;
        return -1;
    }

    std::string binary(int minPrec) {
        std::string lhs = unary();
        for (;;) {
            std::string op;
            if (cur().k == TK::Punct) op = cur().s;
            else if (isId("instanceof") || isId("in")) op = cur().s;
            int p = prec(op);
            if (p < 0 || p < minPrec) break;
            ++p_;
            std::string rhs = binary(op == "**" ? p : p + 1);
            if (op == "+") lhs = "__add(" + lhs + "," + rhs + ")";
            else if (op == "-") lhs = "__sub(" + lhs + "," + rhs + ")";
            else if (op == "*") lhs = "__mul(" + lhs + "," + rhs + ")";
            else if (op == "/") lhs = "__div(" + lhs + "," + rhs + ")";
            else if (op == "instanceof" || op == "in") lhs = "(" + lhs + " " + op + " " + rhs + ")";
            else lhs = "(" + lhs + op + rhs + ")";
        }
        return lhs;
    }

    std::string unary() {
        if (isP("-")) { ++p_; return "__neg(" + unary() + ")"; }
        if (isP("+") || isP("!") || isP("~")) { std::string o = take(); return o + unary(); }
        if (isP("++") || isP("--")) { std::string o = take(); return o + unary(); }
        if (isId("typeof") || isId("void") || isId("delete")) { std::string o = take(); return o + " " + unary(); }
        if (isId("new")) { ++p_; return "new " + postfix(); }
        return postfix();
    }

    std::string postfix() {
        std::string e = primary();
        for (;;) {
            if (isP(".") || isP("?.")) {
                std::string dot = take();
                if (cur().k != TK::Ident) throw Bail();
                e += dot + take();
            } else if (isP("[")) {
                ++p_;
                std::string idx = expression();
                expectP("]");
                e += "[" + idx + "]";
            } else if (isP("(")) {
                ++p_;
                std::string args;
                while (!isP(")")) {
                    if (isP("...")) { ++p_; args += "..."; }
                    args += assignment();
                    if (isP(",")) { ++p_; args += ","; }
                    else if (!isP(")")) throw Bail();
                }
                ++p_;
                e += "(" + args + ")";
            } else if (isP("++") || isP("--")) {
                e += take();
            } else break;
        }
        return e;
    }

    std::string primary() {
        const Tok& t = cur();
        if (t.k == TK::Num || t.k == TK::Str) { ++p_; return t.s; }
        if (t.k == TK::Ident) {
            if (t.s == "function") return "(" + functionExpr() + ")";
            ++p_;
            return t.s;
        }
        if (isP("(")) {
            ++p_;
            std::string e = expression();
            expectP(")");
            return "(" + e + ")";
        }
        if (isP("[")) {
            ++p_;
            std::string out = "[";
            while (!isP("]")) {
                if (isP(",")) { ++p_; out += ","; continue; }
                if (isP("...")) { ++p_; out += "..."; }
                out += assignment();
                if (isP(",")) { ++p_; out += ","; }
                else if (!isP("]")) throw Bail();
            }
            ++p_;
            return out + "]";
        }
        if (isP("{")) {
            ++p_;
            std::string out = "({";
            while (!isP("}")) {
                std::string key;
                if (cur().k == TK::Ident || cur().k == TK::Str || cur().k == TK::Num) key = take();
                else if (isP("[")) { ++p_; key = "[" + expression() + "]"; expectP("]"); }
                else throw Bail();
                if (isP(":")) { ++p_; out += key + ":" + assignment(); }
                else if (isP("(")) throw Bail();  // method shorthand unsupported
                else out += key;  // shorthand {a}
                if (isP(",")) { ++p_; out += ","; }
                else if (!isP("}")) throw Bail();
            }
            ++p_;
            return out + "})";
        }
        throw Bail();
    }
};

}  // namespace

bool rewriteVectorOperators(const std::string& src, std::string& out) {
    try {
        Parser p(tokenize(src));
        out = p.program();
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace mf
