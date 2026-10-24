/* kexpr.cpp - see kexpr.hpp */
#include "kexpr.hpp"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace kn {

enum {
    NUM, VX, VY, VZ, VT, VAR, NEG, ADD, SUB, MUL, DIV, POW,
    SIN, COS, TAN, ASIN, ACOS, ATAN, SINH, COSH, TANH, EXP, LOG, LOG10, SQRT, ABS, FLOOR, CEIL, SIGN, STEP,
    MIN, MAX, ATAN2, IF
};

static const struct {
    const char *name;
    int op, args;
} FUNCS[] = {
    {"sin", SIN, 1},   {"cos", COS, 1},     {"tan", TAN, 1},     {"asin", ASIN, 1}, {"acos", ACOS, 1}, {"atan", ATAN, 1},
    {"sinh", SINH, 1}, {"cosh", COSH, 1},   {"tanh", TANH, 1},   {"exp", EXP, 1},   {"log", LOG, 1},   {"log10", LOG10, 1},
    {"sqrt", SQRT, 1}, {"abs", ABS, 1},     {"floor", FLOOR, 1}, {"ceil", CEIL, 1}, {"sign", SIGN, 1}, {"step", STEP, 1},
    {"min", MIN, 2},   {"max", MAX, 2},     {"pow", POW, 2},     {"atan2", ATAN2, 2}, {"if", IF, 3},
};

int Expr::add(int op, double v, int a, int b, int c) {
    nodes.push_back(Node{op, v, a, b, c});
    return (int)nodes.size() - 1;
}

void Expr::skip() {
    while (pos < src.size() && isspace((unsigned char)src[pos])) pos++;
}

int Expr::slot_node(int s) {
    bool seen = false;
    for (int u : used) seen = seen || u == s;
    if (!seen) used.push_back(s);
    return add(VAR, 0, s);
}

bool Expr::parse(const std::string &text, std::string &err, const Symbols *symbols) {
    src = text, pos = 0, nodes.clear(), used.clear(), error.clear(), sym = symbols;
    root = parse_sum();
    skip();
    if (error.empty() && pos != src.size()) error = "unexpected '" + src.substr(pos, 1) + "'";
    if (!error.empty()) {
        err = "formula \"" + src + "\": " + error + " at character " + std::to_string(pos + 1);
        root = -1;
        return false;
    }
    return true;
}

int Expr::parse_sum() {
    int a = parse_product();
    for (;;) {
        skip();
        if (!error.empty() || pos >= src.size()) return a;
        char c = src[pos];
        if (c != '+' && c != '-') return a;
        pos++;
        int b = parse_product();
        a = add(c == '+' ? ADD : SUB, 0, a, b);
    }
}

int Expr::parse_product() {
    int a = parse_unary();
    for (;;) {
        skip();
        if (!error.empty() || pos >= src.size()) return a;
        char c = src[pos];
        if (c != '*' && c != '/') return a;
        pos++;
        int b = parse_unary();
        a = add(c == '*' ? MUL : DIV, 0, a, b);
    }
}

int Expr::parse_unary() {
    skip();
    if (pos < src.size() && (src[pos] == '-' || src[pos] == '+')) {
        char c = src[pos++];
        int a = parse_unary();
        return c == '-' ? add(NEG, 0, a) : a;
    }
    return parse_power();
}

int Expr::parse_power() {
    int a = parse_primary();
    skip();
    if (error.empty() && pos < src.size() && src[pos] == '^') {
        pos++;
        int b = parse_unary(); /* right associative; 2^-1 allowed */
        return add(POW, 0, a, b);
    }
    return a;
}

int Expr::parse_primary() {
    skip();
    if (!error.empty()) return -1;
    if (pos >= src.size()) {
        error = "the formula ends too soon";
        return -1;
    }
    char c = src[pos];
    if (c == '(') {
        pos++;
        int a = parse_sum();
        skip();
        if (error.empty() && (pos >= src.size() || src[pos] != ')')) error = "a ')' is missing";
        else pos++;
        return a;
    }
    if (isdigit((unsigned char)c) || c == '.') {
        char *end = nullptr;
        double v = strtod(src.c_str() + pos, &end);
        if (end == src.c_str() + pos) {
            error = "a number is malformed";
            return -1;
        }
        pos = (size_t)(end - src.c_str());
        return add(NUM, v);
    }
    if (isalpha((unsigned char)c) || c == '_') {
        size_t s = pos;
        while (pos < src.size() && (isalnum((unsigned char)src[pos]) || src[pos] == '_')) pos++;
        std::string id = src.substr(s, pos - s);
        skip();
        bool call = pos < src.size() && src[pos] == '(';
        if (!call) {
            if (id == "x") return add(VX, 0);
            if (id == "y") return add(VY, 0);
            if (id == "z") return add(VZ, 0);
            if (id == "t") return add(VT, 0);
            if (id == "pi") return add(NUM, M_PI);
            if (id == "e") return add(NUM, M_E);
            if (sym) {
                auto it = sym->slot.find(id);
                if (it != sym->slot.end()) return slot_node(it->second);
            }
            pos = s;
            error = "unknown name '" + id + "' (the variables are x, y, z, t; the constants pi, e";
            if (sym) {
                std::string names;
                for (const auto &kv : sym->slot)
                    if (kv.first.find(':') == std::string::npos) names += (names.empty() ? "" : ", ") + kv.first;
                if (!names.empty()) error += "; and here " + names;
            }
            error += ")";
            return -1;
        }
        if (sym && (id == "dx" || id == "dy" || id == "dz")) { /* a field's gradient: dx(name) */
            pos++;
            skip();
            size_t s2 = pos;
            while (pos < src.size() && (isalnum((unsigned char)src[pos]) || src[pos] == '_')) pos++;
            std::string name = src.substr(s2, pos - s2);
            skip();
            auto it = sym->slot.find(id + ":" + name);
            if (name.empty() || it == sym->slot.end() || pos >= src.size() || src[pos] != ')') {
                pos = s2;
                error = id + "( ) takes the name of a field";
                return -1;
            }
            pos++;
            return slot_node(it->second);
        }
        for (const auto &f : FUNCS) {
            if (id != f.name) continue;
            pos++; /* '(' */
            int arg[3] = {-1, -1, -1};
            for (int k = 0; k < f.args; k++) {
                arg[k] = parse_sum();
                skip();
                if (!error.empty()) return -1;
                char want = k + 1 < f.args ? ',' : ')';
                if (pos >= src.size() || src[pos] != want) {
                    error = std::string("'") + f.name + "' takes " + std::to_string(f.args) + " argument" + (f.args > 1 ? "s" : "") + ": a '" + want + "' is expected";
                    return -1;
                }
                pos++;
            }
            return add(f.op, 0, arg[0], arg[1], arg[2]);
        }
        pos = s;
        error = "unknown function '" + id + "'";
        return -1;
    }
    error = "unexpected '" + std::string(1, c) + "'";
    return -1;
}

double Expr::ev(int n, const double *X, double t, const double *slots) const {
    const Node &d = nodes[(size_t)n];
    switch (d.op) {
    case NUM: return d.value;
    case VX: return X[0];
    case VY: return X[1];
    case VZ: return X[2];
    case VT: return t;
    case VAR: return slots ? slots[d.a] : NAN;
    case NEG: return -ev(d.a, X, t, slots);
    case ADD: return ev(d.a, X, t, slots) + ev(d.b, X, t, slots);
    case SUB: return ev(d.a, X, t, slots) - ev(d.b, X, t, slots);
    case MUL: return ev(d.a, X, t, slots) * ev(d.b, X, t, slots);
    case DIV: return ev(d.a, X, t, slots) / ev(d.b, X, t, slots);
    case POW: return std::pow(ev(d.a, X, t, slots), ev(d.b, X, t, slots));
    case SIN: return std::sin(ev(d.a, X, t, slots));
    case COS: return std::cos(ev(d.a, X, t, slots));
    case TAN: return std::tan(ev(d.a, X, t, slots));
    case ASIN: return std::asin(ev(d.a, X, t, slots));
    case ACOS: return std::acos(ev(d.a, X, t, slots));
    case ATAN: return std::atan(ev(d.a, X, t, slots));
    case SINH: return std::sinh(ev(d.a, X, t, slots));
    case COSH: return std::cosh(ev(d.a, X, t, slots));
    case TANH: return std::tanh(ev(d.a, X, t, slots));
    case EXP: return std::exp(ev(d.a, X, t, slots));
    case LOG: return std::log(ev(d.a, X, t, slots));
    case LOG10: return std::log10(ev(d.a, X, t, slots));
    case SQRT: return std::sqrt(ev(d.a, X, t, slots));
    case ABS: return std::fabs(ev(d.a, X, t, slots));
    case FLOOR: return std::floor(ev(d.a, X, t, slots));
    case CEIL: return std::ceil(ev(d.a, X, t, slots));
    case SIGN: {
        double v = ev(d.a, X, t, slots);
        return (v > 0) - (v < 0);
    }
    case STEP: return ev(d.a, X, t, slots) >= 0 ? 1.0 : 0.0;
    case MIN: return std::fmin(ev(d.a, X, t, slots), ev(d.b, X, t, slots));
    case MAX: return std::fmax(ev(d.a, X, t, slots), ev(d.b, X, t, slots));
    case ATAN2: return std::atan2(ev(d.a, X, t, slots), ev(d.b, X, t, slots));
    case IF: return ev(d.a, X, t, slots) > 0 ? ev(d.b, X, t, slots) : ev(d.c, X, t, slots);
    }
    return NAN;
}

double Expr::eval(const double *xyz, double t, const double *slots) const { return root < 0 ? NAN : ev(root, xyz, t, slots); }

bool Expr::constant() const {
    for (const Node &d : nodes)
        if (d.op == VX || d.op == VY || d.op == VZ || d.op == VT || d.op == VAR) return false;
    return true;
}

bool Expr::uses_position() const {
    for (const Node &d : nodes)
        if (d.op == VX || d.op == VY || d.op == VZ) return true;
    return false;
}

bool Expr::uses_time() const {
    for (const Node &d : nodes)
        if (d.op == VT) return true;
    return false;
}

} // namespace kn
