/* kexpr.hpp - formulas in a scenario: any number the kernel reads may be written as a function of the position (x, y,
 * z, in metres) and the time t (seconds), so that an agent states a load, a material that varies, a boundary value or
 * an exact solution as mathematics instead of code.
 *
 * Grammar: numbers (1, 2.5, 1e-3), x y z t, pi and e, + - * / ^ (power, right associative, binding tighter than a
 * leading minus: -x^2 is -(x^2)), parentheses, and the functions sin cos tan asin acos atan sinh cosh tanh exp log
 * log10 sqrt abs floor ceil sign (one argument), min max pow atan2 (two), step(s) (0 below 0, 1 from 0), if(c, a, b)
 * (a where c > 0, else b). Parsing reports the position of an error.
 *
 * Where the kernel solves equations of its own fields, a formula may also name them (the caller gives the names as
 * Symbols): a field's value by its name, its gradient by dx(name), dy(name), dz(name), and a material's property by
 * its key. Each name is a slot that the caller fills before evaluating. */
#pragma once

#include <map>
#include <string>
#include <vector>

namespace kn {

class Expr {
public:
    /* names beyond x, y, z, t that a formula may use: a name, or "dx:name", "dy:name", "dz:name" for a gradient's
     * component, to the index of its slot */
    struct Symbols {
        std::map<std::string, int> slot;
    };
    /* false with a message naming where the text went wrong */
    bool parse(const std::string &text, std::string &err, const Symbols *symbols = nullptr);
    double eval(const double *xyz, double t = 0, const double *slots = nullptr) const;
    bool constant() const;     /* no x, y, z, t or slot in it */
    bool uses_time() const;    /* t appears in it */
    bool uses_position() const; /* x, y or z appears in it */
    const std::vector<int> &slots_used() const { return used; } /* each slot once */
    const std::string &text() const { return src; }

private:
    struct Node {
        int op;        /* the kinds in kexpr.cpp */
        double value;  /* a number */
        int a, b, c;   /* operands: indices into nodes; a slot's index for a name */
    };
    std::vector<Node> nodes;
    std::vector<int> used;
    int root = -1;
    std::string src;
    size_t pos = 0;
    std::string error;
    const Symbols *sym = nullptr;
    int add(int op, double v, int a = -1, int b = -1, int c = -1);
    int parse_sum();
    int parse_product();
    int parse_unary();
    int parse_power();
    int parse_primary();
    int slot_node(int s);
    void skip();
    double ev(int n, const double *xyz, double t, const double *slots) const;
};

} // namespace kn
