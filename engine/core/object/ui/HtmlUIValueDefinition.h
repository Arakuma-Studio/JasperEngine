// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef HTMLUIVALUEDEFINITION_H
#define HTMLUIVALUEDEFINITION_H

// Engine-internal (RmlUi): RmlUi's view of a UIValue tree (data binding, phase 1 §3.9.4)

#include "Export.h"
#include "UIValue.h"

#include <RmlUi/Core/DataVariable.h>

#include <string>

namespace jasperengine{

    struct HtmlUIDataModelRecord;

    // Get/Set/Size/Child over a UIValue tree, after RmlUi's Lua plugin (LuaObjectDef). One
    // definition per top-level variable: a data-value/data-checked write (Set) is recorded under
    // that variable's name for UIDataModel::onChange. Children are resolved on every access, so
    // no pointer into an array outlives RmlUi's update.
    class JASPERENGINE_INTERNAL HtmlUIValueDefinition final : public Rml::VariableDefinition {
    public:
        HtmlUIValueDefinition(HtmlUIDataModelRecord* record, std::string topLevel);

        bool Get(void* ptr, Rml::Variant& variant) override;
        bool Set(void* ptr, const Rml::Variant& variant) override;
        int Size(void* ptr) override;
        Rml::DataVariable Child(void* ptr, const Rml::DataAddressEntry& address) override;
        Rml::StringList ReflectMemberNames() override;

        // conversions shared with data-event arguments
        static void toVariant(const UIValue& value, Rml::Variant& variant);
        static UIValue fromVariant(const Rml::Variant& variant);

    private:
        HtmlUIDataModelRecord* record;
        std::string topLevel;
    };

}

#endif //HTMLUIVALUEDEFINITION_H
