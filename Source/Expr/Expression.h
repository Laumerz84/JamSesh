#pragma once

#include <juce_core/juce_core.h>
#include <memory>

// Live-editable math formulas, compiled once and evaluated per sample / per point.
//
// Owner: Agent A (implements Expression.cpp). Public API is a contract used by other modules.
//
// Syntax: + - * / ^ (power, right-assoc) %, unary -, parentheses, comparisons < > <= >= == !=
// (result 1 or 0), and/or/not via && || !, ternary a ? b : c.
// Functions: sin cos tan asin acos atan atan2 sinh cosh tanh exp log log2 log10 sqrt abs sign
// floor ceil round fract min max clamp(x,lo,hi) pow mod lerp(a,b,t) step(edge,x) smoothstep(a,b,x)
// sat(x) (soft clip, tanh) tri(x) saw(x) sqr(x) (period 2*pi, range -1..1) hypot.
// Constants: pi tau e.
namespace expr
{
    class Expression
    {
    public:
        // Compiles `text` with the given variable names (case-sensitive). On failure returns
        // nullptr and fills `errorOut` with a short, user-readable message including the
        // character position, e.g. "Unknown name 'sinn' at 5".
        static std::shared_ptr<const Expression> compile (const juce::String& text,
                                                          const juce::StringArray& variables,
                                                          juce::String& errorOut);

        // values[i] is the value of variables[i]. Real-time safe: no allocation, no locks,
        // no exceptions. Non-finite results are returned as 0.
        float evaluate (const float* values) const noexcept;

        const juce::String& source() const noexcept;

        ~Expression();

    private:
        Expression();
        struct Impl;
        std::unique_ptr<Impl> impl;
    };
}
