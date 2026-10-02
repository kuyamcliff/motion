// Expression engine (embedded QuickJS, sandboxed, no I/O bindings).
#pragma once
#include <map>
#include <mutex>

#include "property.hpp"

namespace mf {

struct AudioFeatureSource {
    virtual ~AudioFeatureSource() = default;
    // Features at comp time: amplitude, rms, bass, mid, treble, beat (0..1), tempo (bpm), centroid (Hz)
    virtual bool features(const json* comp, double compTime, std::map<std::string, double>& out) = 0;
};

struct ExpressionEngine {
    virtual ~ExpressionEngine() = default;
    // Evaluate expression for a property. `value` is the pre-expression value.
    virtual bool evaluate(const std::string& src, const Value& value, const json& prop, const EvalContext& ctx, Value& out,
                          std::string& err) = 0;
    // Syntax check without evaluation context.
    virtual bool validate(const std::string& src, std::string& err) = 0;
    // Most recent error per "layerId:propPath".
    std::map<std::string, std::string> errors() {
        std::lock_guard<std::mutex> lk(errMutex);
        return lastErrors;
    }
    void clearErrors() {
        std::lock_guard<std::mutex> lk(errMutex);
        lastErrors.clear();
    }

   protected:
    void recordError(const std::string& key, const std::string& e) {
        std::lock_guard<std::mutex> lk(errMutex);
        if (e.empty()) lastErrors.erase(key); else lastErrors[key] = e;
    }
    std::mutex errMutex;
    std::map<std::string, std::string> lastErrors;
};

// Creates a QuickJS-backed expression engine. Each instance must be used from one thread at a time.
std::unique_ptr<ExpressionEngine> createExpressionEngine();

// Rewrites vector arithmetic (a + b on arrays) into helper calls so that
// After-Effects-style expressions like `value + [10, 0]` work in plain JS.
// Returns false if the source uses syntax outside the supported subset (caller then runs it verbatim).
bool rewriteVectorOperators(const std::string& src, std::string& out);

}  // namespace mf
