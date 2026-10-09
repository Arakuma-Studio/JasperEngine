// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/css/BaseRules.h"
#include "htmlui/compat/LiteralCompare.h"

#include "htmlui/compat/css/RewriteSession.h"

#include <algorithm>

using namespace jasperengine::htmlui::compat;

void BaseRules::index(Session& session){
    session.baseIndex.clear();
    for (const FlatRule& rule : session.rules){
        if (rule.origin != RuleOrigin::Author || rule.dropped)
            continue;
        for (const ComplexSelector& selector : rule.selectors){
            std::vector<uint32_t>& list = session.baseIndex[SelectorRewriter::serialize(selector)];
            if (list.empty() || list.back() != rule.index)
                list.push_back(rule.index);
        }
    }
}

const FlatRule* BaseRules::find(Session& session, const FlatRule& rule, const std::function<bool(const FlatRule&)>& sets){
    const FlatRule* best = nullptr;
    uint32_t ruleSpecificity = 0;
    for (const RewrittenSelector& s : rule.rewritten)
        ruleSpecificity = std::max(ruleSpecificity, s.specificity);
    for (const ComplexSelector& selector : rule.selectors){
        const std::string key = SelectorRewriter::baseKey(selector);
        const auto it = session.baseIndex.find(key);
        if (it == session.baseIndex.end())
            continue;
        for (uint32_t index : it->second){
            const FlatRule& candidate = session.rules[index];
            if (candidate.index == rule.index || candidate.dropped)
                continue;
            uint32_t candidateSpecificity = 0;
            for (const RewrittenSelector& s : candidate.rewritten)
                candidateSpecificity = std::max(candidateSpecificity, s.specificity);
            // before the rule in the cascade, or after it with a lower specificity
            if (!(candidate.order < rule.order || candidateSpecificity < ruleSpecificity))
                continue;
            if (!sets(candidate))
                continue;
            if (!best || candidate.order > best->order)
                best = &candidate;
        }
    }
    return best;
}

std::string BaseRules::location(Session& session, const FlatRule& rule){
    const SourcePos pos = session.files.position(rule.ref.file, rule.ref.offset);
    return session.files.path(rule.ref.file) + ":" + std::to_string(pos.line);
}
