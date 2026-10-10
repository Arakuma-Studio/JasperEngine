// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/css/BaseRules.h"
#include "htmlui/compat/LiteralCompare.h"

#include "htmlui/compat/css/RewriteSession.h"

#include <algorithm>

using namespace jasperengine::htmlui::compat;

namespace {

    // the index of BaseRules::index: the rules' origin, selectors and `dropped` are final once
    // the selectors are rewritten, so building it at the first lookup gives the same index
    constexpr uint32_t kNoEntry = 0xFFFFFFFFu;

    // the entry of a selector hash (nullptr when no rule has it and `add` is false)
    Session::BaseEntry* entryOf(Session& session, uint64_t hash, bool add){
        if (!session.baseSlots.empty()){
            const size_t mask = session.baseSlots.size() - 1;
            for (size_t i = (size_t)hash & mask; session.baseSlots[i] != kNoEntry; i = (i + 1) & mask){
                Session::BaseEntry& entry = session.baseEntries[session.baseSlots[i]];
                if (entry.hash == hash)
                    return &entry;
            }
        }
        if (!add)
            return nullptr;
        if ((session.baseEntries.size() + 1) * 2 > session.baseSlots.size()){
            session.baseSlots.assign(session.baseSlots.empty() ? 256 : session.baseSlots.size() * 2, kNoEntry);
            const size_t mask = session.baseSlots.size() - 1;
            for (uint32_t index = 0; index < (uint32_t)session.baseEntries.size(); index++){
                size_t i = (size_t)session.baseEntries[index].hash & mask;
                while (session.baseSlots[i] != kNoEntry)
                    i = (i + 1) & mask;
                session.baseSlots[i] = index;
            }
        }
        const size_t mask = session.baseSlots.size() - 1;
        size_t i = (size_t)hash & mask;
        while (session.baseSlots[i] != kNoEntry)
            i = (i + 1) & mask;
        session.baseSlots[i] = (uint32_t)session.baseEntries.size();
        Session::BaseEntry& entry = session.baseEntries.emplace_back();
        entry.hash = hash;
        return &entry;
    }

    // the index of BaseRules::index: the rules' origin, selectors and `dropped` are final once
    // the selectors are rewritten, so building it at the first lookup gives the same index
    void build(Session& session){
        session.baseIndexBuilt = true;
        session.baseEntries.clear();
        session.baseSlots.clear();
        session.baseEntries.reserve(session.rules.size());
        {
            // (sized for a selector a rule and a half: no rehash while it is built)
            size_t slots = 256;
            while (slots < session.rules.size() * 3)
                slots *= 2;
            session.baseSlots.assign(slots, kNoEntry);
        }
        for (const FlatRule& rule : session.rules){
            if (rule.origin != RuleOrigin::Author || rule.dropped)
                continue;
            for (const ComplexSelector& selector : rule.selectors){
                ArenaList<uint32_t>& list = entryOf(session, SelectorRewriter::textHash(selector, false), true)->rules;
                if (list.empty() || list.back() != rule.index)
                    list.push_back(rule.index);
            }
        }
    }

}

void BaseRules::index(Session& session){
    session.baseEntries.clear();
    session.baseSlots.clear();
    session.baseIndexBuilt = false;
}

const FlatRule* BaseRules::find(Session& session, const FlatRule& rule, const std::function<bool(const FlatRule&)>& sets){
    if (!session.baseIndexBuilt)
        build(session);
    const FlatRule* best = nullptr;
    uint32_t ruleSpecificity = 0;
    for (const RewrittenSelector& s : rule.rewritten)
        ruleSpecificity = std::max(ruleSpecificity, s.specificity);
    std::string& key = session.baseKeyText;
    std::string& text = session.baseCandidateText;
    for (const ComplexSelector& selector : rule.selectors){
        const Session::BaseEntry* entry = entryOf(session, SelectorRewriter::textHash(selector, true), false);
        if (!entry)
            continue;
        key.clear();
        SelectorRewriter::appendBaseKey(key, selector);
        for (uint32_t index : entry->rules){
            const FlatRule& candidate = session.rules[index];
            if (candidate.index == rule.index || candidate.dropped)
                continue;
            // (the hash found it: one of its selectors has the key's text)
            bool same = false;
            for (const ComplexSelector& other : candidate.selectors){
                text.clear();
                SelectorRewriter::appendSerialized(text, other);
                if (text == key){
                    same = true;
                    break;
                }
            }
            if (!same)
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
