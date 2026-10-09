// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/css/Nesting.h"
#include "htmlui/compat/LiteralCompare.h"

using namespace jasperengine::htmlui::compat;

namespace {

    using Kind = SimpleSelector::Kind;

    bool compoundHasNesting(const Compound& compound){
        for (const SimpleSelector& s : compound.simples){
            if (s.kind == Kind::Nesting)
                return true;
        }
        return false;
    }

    // the parent's compounds with `others` merged into its subject compound
    std::vector<Compound> parentWith(const ComplexSelector& parent, const std::vector<SimpleSelector>& others, char combinator){
        std::vector<Compound> result = parent.compounds;
        if (result.empty())
            return result;
        result.front().combinator = combinator;
        Compound& last = result.back();
        std::vector<SimpleSelector> types, rest;
        for (const SimpleSelector& s : last.simples)
            (s.kind == Kind::Type || s.kind == Kind::Universal ? types : rest).push_back(s);
        for (const SimpleSelector& s : others){
            if (s.kind == Kind::Type){
                // "div&" when the parent has a type too: the selector cannot match two types;
                // keep both, it never matches, as in CSS
                types.push_back(s);
            }else if (s.kind != Kind::Universal){
                rest.push_back(s);
            }
        }
        last.simples = types;
        last.simples.insert(last.simples.end(), rest.begin(), rest.end());
        return result;
    }

}

bool Nesting::hasNestingSelector(const ComplexSelector& selector){
    for (const Compound& compound : selector.compounds){
        if (compoundHasNesting(compound))
            return true;
    }
    return false;
}

ComplexSelector Nesting::substitute(const ComplexSelector& parent, const ComplexSelector& child){
    ComplexSelector result;
    result.ref = child.ref;
    if (!hasNestingSelector(child)){
        // implicit `& ` (or `& <combinator>` for a relative selector)
        result.compounds = parent.compounds;
        for (size_t i = 0; i < child.compounds.size(); i++){
            Compound compound = child.compounds[i];
            if (i == 0)
                compound.combinator = (child.relative && compound.combinator) ? compound.combinator : ' ';
            result.compounds.push_back(std::move(compound));
        }
        return result;
    }
    for (size_t i = 0; i < child.compounds.size(); i++){
        const Compound& compound = child.compounds[i];
        if (!compoundHasNesting(compound)){
            Compound copy = compound;
            if (result.compounds.empty())
                copy.combinator = 0;
            result.compounds.push_back(std::move(copy));
            continue;
        }
        std::vector<SimpleSelector> others;
        for (const SimpleSelector& s : compound.simples){
            if (s.kind != Kind::Nesting)
                others.push_back(s);
        }
        const char combinator = result.compounds.empty() ? 0 : (compound.combinator ? compound.combinator : ' ');
        std::vector<Compound> merged = parentWith(parent, others, combinator);
        for (Compound& c : merged)
            result.compounds.push_back(std::move(c));
    }
    return result;
}

std::optional<std::vector<ComplexSelector>> Nesting::combine(const std::vector<ComplexSelector>& parents,
                                                             const std::vector<ComplexSelector>& children, const ForkFeatures& fork){
    std::vector<ComplexSelector> result;
    if (parents.size() > 1 && fork.has(ForkFeature::RootIsWhere)){
        // & = :is(parent list)
        ComplexSelector is;
        Compound compound;
        SimpleSelector simple;
        simple.kind = Kind::PseudoClass;
        simple.name = "is";
        simple.function = true;
        simple.selectors = parents;
        compound.simples.push_back(simple);
        is.compounds.push_back(compound);
        for (const ComplexSelector& child : children)
            result.push_back(substitute(is, child));
        return result;
    }
    if (parents.size() * children.size() > kLimit)
        return std::nullopt;
    for (const ComplexSelector& parent : parents){
        for (const ComplexSelector& child : children)
            result.push_back(substitute(parent, child));
    }
    return result;
}
