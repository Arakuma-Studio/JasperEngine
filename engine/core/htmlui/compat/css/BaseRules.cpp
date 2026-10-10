// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/css/BaseRules.h"
#include "htmlui/compat/LiteralCompare.h"

#include "htmlui/compat/StringIndex.h"
#include "htmlui/compat/css/RewriteSession.h"

#include <algorithm>

using namespace jasperengine::htmlui::compat;

namespace {

    // the index of BaseRules::index: the rules' origin, selectors and `dropped` are final once
    // the selectors are rewritten, so building it at the first lookup gives the same index
    constexpr uint32_t kNoEntry = 0xFFFFFFFFu;

    // the entry of a selector hash (nullptr when no rule has it and `add` is false)
    Session::BaseEntry* entryOf(Session& session, uint64_t hash, bool add){
        if (add && (session.baseEntries.size() + 1) * 2 > session.baseSlots.size()){
            session.baseSlots.assign(session.baseSlots.empty() ? 256 : session.baseSlots.size() * 2, kNoEntry);
            const size_t mask = session.baseSlots.size() - 1;
            for (uint32_t index = 0; index < (uint32_t)session.baseEntries.size(); index++){
                size_t i = (size_t)session.baseEntries[index].hash & mask;
                while (session.baseSlots[i] != kNoEntry)
                    i = (i + 1) & mask;
                session.baseSlots[i] = index;
            }
        }
        if (session.baseSlots.empty())
            return nullptr;
        // one probe: the entry, or the free slot it goes to
        const size_t mask = session.baseSlots.size() - 1;
        size_t i = (size_t)hash & mask;
        for (; session.baseSlots[i] != kNoEntry; i = (i + 1) & mask){
            Session::BaseEntry& entry = session.baseEntries[session.baseSlots[i]];
            if (entry.hash == hash)
                return &entry;
        }
        if (!add)
            return nullptr;
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
        std::string& buffer = session.baseCandidateText;
        for (const FlatRule& rule : session.rules){
            if (rule.origin != RuleOrigin::Author || rule.dropped)
                continue;
            for (size_t i = 0; i < rule.selectorCount(); i++){
                ArenaList<uint32_t>& list = entryOf(session, StringIndex::hash(rule.selectorText(i, buffer)), true)->rules;
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
    for (size_t i = 0; i < rule.selectorCount(); i++){
        key.clear();
        if (rule.scanned)
            SelectorRewriter::appendBaseKey(key, rule.selectorTexts[i].view());
        else
            SelectorRewriter::appendBaseKey(key, rule.selectors()[i]);
        const Session::BaseEntry* entry = entryOf(session, StringIndex::hash(key), false);
        if (!entry)
            continue;
        for (uint32_t index : entry->rules){
            const FlatRule& candidate = session.rules[index];
            if (candidate.index == rule.index || candidate.dropped)
                continue;
            // (the hash found it: one of its selectors has the key's text)
            bool same = false;
            for (size_t j = 0; j < candidate.selectorCount() && !same; j++)
                same = candidate.selectorText(j, text) == key;
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
    return session.files.path(rule.ref.file) + ":" + std::to_string(session.files.line(rule.ref.file, rule.ref.offset));
}
