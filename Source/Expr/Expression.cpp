#include "Expression.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace expr
{
namespace
{
    constexpr int maxStack = 64;
    constexpr int maxNesting = 200;

    enum Op : std::uint8_t
    {
        opConst, opVar,
        opNeg, opNot,
        opAdd, opSub, opMul, opDiv, opMod, opPow,
        opLt, opGt, opLe, opGe, opEq, opNe, opAnd, opOr,
        opSelect,
        opSin, opCos, opTan, opAsin, opAcos, opAtan, opSinh, opCosh, opTanh, opExp, opLog, opLog2, opLog10,
        opSqrt, opAbs, opSign, opFloor, opCeil, opRound, opFract, opSat, opTri, opSaw, opSqr,
        opSquare, opCube,   // x^2 and x^3 rewritten from pow (much cheaper than std::pow)
        opAtan2, opMin, opMax, opStep, opHypot,
        opClamp, opLerp, opSmoothstep
    };

    inline int arityOf (Op op) noexcept
    {
        if (op <= opVar)       return 0;
        if (op <= opNot)       return 1;
        if (op <= opOr)        return 2;
        if (op == opSelect)    return 3;
        if (op <= opCube)      return 1;
        if (op <= opHypot)     return 2;
        return 3;
    }

    inline float fractf (float x) noexcept { return x - std::floor (x); }

    inline float floorMod (float a, float b) noexcept
    {
        return b == 0.0f ? 0.0f : a - b * std::floor (a / b);
    }

    constexpr float invTwoPi = 0.15915494309189535f;

    inline float apply (Op op, float a, float b, float c) noexcept
    {
        switch (op)
        {
            case opNeg:   return -a;
            case opNot:   return a == 0.0f ? 1.0f : 0.0f;
            case opAdd:   return a + b;
            case opSub:   return a - b;
            case opMul:   return a * b;
            case opDiv:   return a / b;
            case opMod:   return floorMod (a, b);
            case opPow:   return std::pow (a, b);
            case opLt:    return a <  b ? 1.0f : 0.0f;
            case opGt:    return a >  b ? 1.0f : 0.0f;
            case opLe:    return a <= b ? 1.0f : 0.0f;
            case opGe:    return a >= b ? 1.0f : 0.0f;
            case opEq:    return a == b ? 1.0f : 0.0f;
            case opNe:    return a != b ? 1.0f : 0.0f;
            case opAnd:   return (a != 0.0f && b != 0.0f) ? 1.0f : 0.0f;
            case opOr:    return (a != 0.0f || b != 0.0f) ? 1.0f : 0.0f;
            case opSelect: return a != 0.0f ? b : c;
            case opSin:   return std::sin (a);
            case opCos:   return std::cos (a);
            case opTan:   return std::tan (a);
            case opAsin:  return std::asin (a);
            case opAcos:  return std::acos (a);
            case opAtan:  return std::atan (a);
            case opSinh:  return std::sinh (a);
            case opCosh:  return std::cosh (a);
            case opTanh:  return std::tanh (a);
            case opExp:   return std::exp (a);
            case opLog:   return std::log (a);
            case opLog2:  return std::log2 (a);
            case opLog10: return std::log10 (a);
            case opSqrt:  return std::sqrt (a);
            case opAbs:   return std::abs (a);
            case opSign:  return a > 0.0f ? 1.0f : (a < 0.0f ? -1.0f : 0.0f);
            case opFloor: return std::floor (a);
            case opCeil:  return std::ceil (a);
            case opRound: return std::round (a);
            case opFract: return fractf (a);
            case opSat:   return std::tanh (a);
            // periodic shapes: period 2*pi, range -1..1, phase-aligned with sin
            case opTri:   return 4.0f * std::abs (fractf (a * invTwoPi - 0.25f) - 0.5f) - 1.0f;
            case opSaw:   return 2.0f * fractf (a * invTwoPi + 0.5f) - 1.0f;
            case opSqr:   return fractf (a * invTwoPi) < 0.5f ? 1.0f : -1.0f;
            case opSquare: return a * a;
            case opCube:  return a * a * a;
            case opAtan2: return std::atan2 (a, b);
            case opMin:   return a < b ? a : b;
            case opMax:   return a > b ? a : b;
            case opStep:  return b >= a ? 1.0f : 0.0f;
            case opHypot: return std::sqrt (a * a + b * b);
            case opClamp: return a < b ? b : (a > c ? c : a);
            case opLerp:  return a + (b - a) * c;
            case opSmoothstep:
            {
                const float t0 = (b - a) == 0.0f ? 0.0f : (c - a) / (b - a);
                const float t = t0 < 0.0f ? 0.0f : (t0 > 1.0f ? 1.0f : t0);
                return t * t * (3.0f - 2.0f * t);
            }
            default:      return 0.0f;
        }
    }

    struct FuncDef { const char* name; Op op; };

    const FuncDef functions[] = {
        { "sin", opSin }, { "cos", opCos }, { "tan", opTan }, { "asin", opAsin }, { "acos", opAcos },
        { "atan", opAtan }, { "sinh", opSinh }, { "cosh", opCosh }, { "tanh", opTanh }, { "exp", opExp },
        { "log", opLog }, { "log2", opLog2 }, { "log10", opLog10 }, { "sqrt", opSqrt }, { "abs", opAbs },
        { "sign", opSign }, { "floor", opFloor }, { "ceil", opCeil }, { "round", opRound }, { "fract", opFract },
        { "sat", opSat }, { "tri", opTri }, { "saw", opSaw }, { "sqr", opSqr },
        { "atan2", opAtan2 }, { "min", opMin }, { "max", opMax }, { "pow", opPow }, { "mod", opMod },
        { "step", opStep }, { "hypot", opHypot },
        { "clamp", opClamp }, { "lerp", opLerp }, { "smoothstep", opSmoothstep },
    };

    // rhs: 0 = right operand is on the stack, 1 = constant `value`, 2 = variable `var`.
    // Folding leaf operands into binary instructions roughly halves the dispatch count.
    struct Instr
    {
        Op op;
        std::uint8_t arity;
        std::uint8_t rhs;
        int var;
        float value;
    };

    struct Node
    {
        Op op = opConst;
        float value = 0.0f;
        int var = -1;
        int kids[3] { -1, -1, -1 };
    };

    enum TokType { tNum, tName, tOp, tEnd };

    struct Token
    {
        TokType type;
        std::string text;
        float value;
        int pos;   // 1-based character position
    };

    bool tokenize (const std::string& s, std::vector<Token>& out, juce::String& err)
    {
        size_t i = 0;
        while (i < s.size())
        {
            const char ch = s[i];
            const int pos = (int) i + 1;
            if (std::isspace ((unsigned char) ch)) { ++i; continue; }

            if (std::isdigit ((unsigned char) ch) || (ch == '.' && i + 1 < s.size() && std::isdigit ((unsigned char) s[i + 1])))
            {
                size_t j = i;
                while (j < s.size() && (std::isdigit ((unsigned char) s[j]) || s[j] == '.')) ++j;
                // exponent only when digits follow, so "2e3" is a number and "2*e" still means the constant
                if (j < s.size() && (s[j] == 'e' || s[j] == 'E'))
                {
                    size_t k = j + 1;
                    if (k < s.size() && (s[k] == '+' || s[k] == '-')) ++k;
                    if (k < s.size() && std::isdigit ((unsigned char) s[k]))
                    {
                        while (k < s.size() && std::isdigit ((unsigned char) s[k])) ++k;
                        j = k;
                    }
                }
                const std::string num = s.substr (i, j - i);
                char* end = nullptr;
                const double v = std::strtod (num.c_str(), &end);
                if (end == nullptr || *end != '\0')
                {
                    err = "Bad number '" + juce::String (num) + "' at " + juce::String (pos);
                    return false;
                }
                out.push_back ({ tNum, num, (float) v, pos });
                i = j;
                continue;
            }

            if (std::isalpha ((unsigned char) ch) || ch == '_')
            {
                size_t j = i;
                while (j < s.size() && (std::isalnum ((unsigned char) s[j]) || s[j] == '_')) ++j;
                out.push_back ({ tName, s.substr (i, j - i), 0.0f, pos });
                i = j;
                continue;
            }

            static const char* twoChar[] = { "<=", ">=", "==", "!=", "&&", "||", "**" };
            bool matched = false;
            for (auto* op : twoChar)
                if (s.compare (i, 2, op) == 0)
                {
                    out.push_back ({ tOp, op, 0.0f, pos });
                    i += 2;
                    matched = true;
                    break;
                }
            if (matched)
                continue;

            if (std::string ("+-*/%^(),?:<>!").find (ch) != std::string::npos)
            {
                out.push_back ({ tOp, std::string (1, ch), 0.0f, pos });
                ++i;
                continue;
            }

            err = "Unexpected character '" + juce::String::charToString ((juce::juce_wchar) (unsigned char) ch)
                  + "' at " + juce::String (pos);
            return false;
        }
        out.push_back ({ tEnd, "", 0.0f, (int) s.size() + 1 });
        return true;
    }

    class Parser
    {
    public:
        Parser (const std::vector<Token>& t, const juce::StringArray& vars) : toks (t), variables (vars) {}

        int parse()
        {
            const int root = ternary();
            if (failed()) return -1;
            if (peek().type != tEnd)
                return fail ("Unexpected '" + juce::String (peek().text) + "' at " + juce::String (peek().pos));
            return root;
        }

        bool failed() const { return error.isNotEmpty(); }

        juce::String error;
        std::vector<Node> nodes;

    private:
        const std::vector<Token>& toks;
        const juce::StringArray& variables;
        size_t index = 0;
        int depth = 0;

        const Token& peek() const { return toks[index]; }
        bool isOp (const char* op) const { return peek().type == tOp && peek().text == op; }
        bool accept (const char* op) { if (isOp (op)) { ++index; return true; } return false; }

        int fail (const juce::String& msg)
        {
            if (error.isEmpty()) error = msg;
            return -1;
        }

        void expect (const char* op)
        {
            if (accept (op)) return;
            if (peek().type == tEnd)
                fail ("Expected '" + juce::String (op) + "' but the formula ended");
            else
                fail ("Expected '" + juce::String (op) + "' at " + juce::String (peek().pos));
        }

        int make (Op op, int a = -1, int b = -1, int c = -1, float value = 0.0f, int var = -1)
        {
            // strength reduction: x^1, x^2, x^3, x^0.5 with a constant exponent
            if (op == opPow && nodes[(size_t) b].op == opConst && nodes[(size_t) a].op != opConst)
            {
                const float ex = nodes[(size_t) b].value;
                if (ex == 1.0f) return a;
                if (ex == 2.0f) return make (opSquare, a);
                if (ex == 3.0f) return make (opCube, a);
                if (ex == 0.5f) return make (opSqrt, a);
            }

            Node n;
            n.op = op; n.value = value; n.var = var;
            n.kids[0] = a; n.kids[1] = b; n.kids[2] = c;

            // constant folding: when every child is a constant, evaluate now
            const int ar = arityOf (op);
            if (ar > 0)
            {
                bool allConst = true;
                float args[3] { 0.0f, 0.0f, 0.0f };
                for (int k = 0; k < ar; ++k)
                {
                    if (nodes[(size_t) n.kids[k]].op != opConst) { allConst = false; break; }
                    args[k] = nodes[(size_t) n.kids[k]].value;
                }
                if (allConst)
                {
                    Node folded;
                    folded.value = apply (op, args[0], args[1], args[2]);
                    nodes.push_back (folded);
                    return (int) nodes.size() - 1;
                }
            }
            nodes.push_back (n);
            return (int) nodes.size() - 1;
        }

        struct DepthGuard
        {
            explicit DepthGuard (int& d) : ref (d) { ++ref; }
            ~DepthGuard() { --ref; }
            int& ref;
        };

        int ternary()
        {
            DepthGuard g (depth);
            if (depth > maxNesting) return fail ("The formula is nested too deeply");

            int cond = logicOr();
            if (failed()) return -1;
            if (accept ("?"))
            {
                const int a = ternary();
                if (failed()) return -1;
                expect (":");
                if (failed()) return -1;
                const int b = ternary();
                if (failed()) return -1;
                cond = make (opSelect, cond, a, b);
            }
            return cond;
        }

        int logicOr()
        {
            int lhs = logicAnd();
            while (! failed() && accept ("||"))
            {
                const int rhs = logicAnd();
                if (failed()) return -1;
                lhs = make (opOr, lhs, rhs);
            }
            return lhs;
        }

        int logicAnd()
        {
            int lhs = equality();
            while (! failed() && accept ("&&"))
            {
                const int rhs = equality();
                if (failed()) return -1;
                lhs = make (opAnd, lhs, rhs);
            }
            return lhs;
        }

        int equality()
        {
            int lhs = relational();
            while (! failed())
            {
                Op op;
                if (accept ("==")) op = opEq;
                else if (accept ("!=")) op = opNe;
                else break;
                const int rhs = relational();
                if (failed()) return -1;
                lhs = make (op, lhs, rhs);
            }
            return lhs;
        }

        int relational()
        {
            int lhs = additive();
            while (! failed())
            {
                Op op;
                if (accept ("<=")) op = opLe;
                else if (accept (">=")) op = opGe;
                else if (accept ("<")) op = opLt;
                else if (accept (">")) op = opGt;
                else break;
                const int rhs = additive();
                if (failed()) return -1;
                lhs = make (op, lhs, rhs);
            }
            return lhs;
        }

        int additive()
        {
            int lhs = multiplicative();
            while (! failed())
            {
                Op op;
                if (accept ("+")) op = opAdd;
                else if (accept ("-")) op = opSub;
                else break;
                const int rhs = multiplicative();
                if (failed()) return -1;
                lhs = make (op, lhs, rhs);
            }
            return lhs;
        }

        int multiplicative()
        {
            int lhs = unary();
            while (! failed())
            {
                Op op;
                if (accept ("*")) op = opMul;
                else if (accept ("/")) op = opDiv;
                else if (accept ("%")) op = opMod;
                else break;
                const int rhs = unary();
                if (failed()) return -1;
                lhs = make (op, lhs, rhs);
            }
            return lhs;
        }

        int unary()
        {
            DepthGuard g (depth);
            if (depth > maxNesting) return fail ("The formula is nested too deeply");

            if (accept ("-")) { const int a = unary(); return failed() ? -1 : make (opNeg, a); }
            if (accept ("+")) return unary();
            if (accept ("!")) { const int a = unary(); return failed() ? -1 : make (opNot, a); }
            return power();
        }

        // right-associative and tighter than a unary minus on its left: -x^2 = -(x^2), 2^3^2 = 2^9
        int power()
        {
            const int base = primary();
            if (failed()) return -1;
            if (accept ("^") || accept ("**"))
            {
                const int ex = unary();
                if (failed()) return -1;
                return make (opPow, base, ex);
            }
            return base;
        }

        int primary()
        {
            const Token& tok = peek();

            if (tok.type == tNum)
            {
                ++index;
                return make (opConst, -1, -1, -1, tok.value);
            }

            if (tok.type == tName)
            {
                ++index;
                const juce::String name (tok.text);
                const int namePos = tok.pos;

                if (isOp ("("))
                {
                    ++index;
                    const FuncDef* def = nullptr;
                    for (auto& f : functions)
                        if (name == f.name) { def = &f; break; }
                    if (def == nullptr)
                        return fail ("Unknown function '" + name + "' at " + juce::String (namePos));

                    int args[3] { -1, -1, -1 };
                    int count = 0;
                    if (! isOp (")"))
                    {
                        for (;;)
                        {
                            const int a = ternary();
                            if (failed()) return -1;
                            if (count < 3) args[count] = a;
                            ++count;
                            if (! accept (",")) break;
                        }
                    }
                    expect (")");
                    if (failed()) return -1;

                    const int want = arityOf (def->op);
                    if (count != want)
                        return fail ("'" + name + "' needs " + juce::String (want) + (want == 1 ? " argument" : " arguments")
                                     + " (got " + juce::String (count) + ") at " + juce::String (namePos));
                    return make (def->op, args[0], args[1], args[2]);
                }

                const int v = variables.indexOf (name);
                if (v >= 0)
                    return make (opVar, -1, -1, -1, 0.0f, v);
                if (name == "pi")  return make (opConst, -1, -1, -1, 3.14159265358979f);
                if (name == "tau") return make (opConst, -1, -1, -1, 6.28318530717959f);
                if (name == "e")   return make (opConst, -1, -1, -1, 2.71828182845905f);

                for (auto& f : functions)
                    if (name == f.name)
                        return fail ("'" + name + "' is a function, write " + name + "(...) at " + juce::String (namePos));
                return fail ("Unknown name '" + name + "' at " + juce::String (namePos));
            }

            if (tok.type == tOp && tok.text == "(")
            {
                ++index;
                const int inner = ternary();
                if (failed()) return -1;
                expect (")");
                return failed() ? -1 : inner;
            }

            if (tok.type == tEnd)
                return fail ("The formula ends too early");
            return fail ("Unexpected '" + juce::String (tok.text) + "' at " + juce::String (tok.pos));
        }
    };

    bool emit (const std::vector<Node>& nodes, int index, std::vector<Instr>& code, int& depth, int& maxDepth)
    {
        const Node& n = nodes[(size_t) index];
        const int ar = arityOf (n.op);

        if (ar == 2)
        {
            const Node& rhs = nodes[(size_t) n.kids[1]];
            if (rhs.op == opConst || rhs.op == opVar)
            {
                if (! emit (nodes, n.kids[0], code, depth, maxDepth))
                    return false;
                code.push_back ({ n.op, (std::uint8_t) 2, (std::uint8_t) (rhs.op == opConst ? 1 : 2), rhs.var, rhs.value });
                return true;   // pops one, pushes one
            }
        }

        for (int k = 0; k < ar; ++k)
            if (! emit (nodes, n.kids[k], code, depth, maxDepth))
                return false;

        code.push_back ({ n.op, (std::uint8_t) ar, (std::uint8_t) 0, n.var, n.value });
        depth += 1 - ar;
        maxDepth = std::max (maxDepth, depth);
        return maxDepth <= maxStack;
    }
}

struct Expression::Impl
{
    std::vector<Instr> code;
    juce::String source;
};

Expression::Expression() : impl (std::make_unique<Impl>()) {}
Expression::~Expression() = default;

const juce::String& Expression::source() const noexcept { return impl->source; }

std::shared_ptr<const Expression> Expression::compile (const juce::String& text,
                                                       const juce::StringArray& variables,
                                                       juce::String& errorOut)
{
    errorOut.clear();
    const std::string src = text.toStdString();

    std::vector<Token> toks;
    if (! tokenize (src, toks, errorOut))
        return nullptr;
    if (toks.size() == 1)
    {
        errorOut = "The formula is empty";
        return nullptr;
    }

    Parser parser (toks, variables);
    const int root = parser.parse();
    if (parser.failed() || root < 0)
    {
        errorOut = parser.error.isNotEmpty() ? parser.error : juce::String ("Could not read the formula");
        return nullptr;
    }

    if (parser.nodes.size() > 4096)
    {
        errorOut = "The formula is too long";
        return nullptr;
    }

    std::shared_ptr<Expression> result (new Expression());
    int depth = 0, maxDepth = 0;
    if (! emit (parser.nodes, root, result->impl->code, depth, maxDepth))
    {
        errorOut = "The formula is too complex (needs more than " + juce::String (maxStack) + " stack slots)";
        return nullptr;
    }
    result->impl->source = text;
    return result;
}

float Expression::evaluate (const float* values) const noexcept
{
    float stack[maxStack];
    int sp = 0;
    for (const auto& in : impl->code)
    {
        switch (in.op)
        {
            case opConst: stack[sp++] = in.value; break;
            case opVar:   stack[sp++] = values[in.var]; break;
            default:
            {
                if (in.rhs != 0)
                {
                    float& a = stack[sp - 1];
                    a = apply (in.op, a, in.rhs == 1 ? in.value : values[in.var], 0.0f);
                    break;
                }
                const int n = in.arity;
                sp -= n;
                const float* a = stack + sp;
                stack[sp++] = apply (in.op, a[0], n > 1 ? a[1] : 0.0f, n > 2 ? a[2] : 0.0f);
                break;
            }
        }
    }
    const float r = sp > 0 ? stack[sp - 1] : 0.0f;
    return std::isfinite (r) ? r : 0.0f;
}
}
