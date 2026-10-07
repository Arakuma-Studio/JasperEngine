// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "HtmlUIValueDefinition.h"
#include "HtmlUIScriptInternal.h"

#include <RmlUi/Core/DataTypes.h>
#include <RmlUi/Core/Variant.h>

#include <cmath>
#include <cstdlib>

using namespace jasperengine;

HtmlUIValueDefinition::HtmlUIValueDefinition(HtmlUIDataModelRecord* record, std::string topLevel):
    Rml::VariableDefinition(Rml::DataVariableType::Scalar), record(record), topLevel(std::move(topLevel)){
}

void HtmlUIValueDefinition::toVariant(const UIValue& value, Rml::Variant& variant){
    switch (value.getType()){
        case UIValue::Type::BOOL:
            variant = value.asBool();
            break;
        case UIValue::Type::NUMBER: {
            // integral numbers print as integers ({{ gold }} -> "10")
            const double number = value.asNumber();
            if (std::floor(number) == number && std::fabs(number) < 9007199254740992.0){
                variant = (int64_t)number;
            }else{
                variant = number;
            }
            break;
        }
        case UIValue::Type::STRING:
            variant = value.asString();
            break;
        default:
            // nil, arrays and objects have no scalar value
            variant.Clear();
            break;
    }
}

UIValue HtmlUIValueDefinition::fromVariant(const Rml::Variant& variant){
    switch (variant.GetType()){
        case Rml::Variant::NONE:
            return UIValue();
        case Rml::Variant::BOOL:
            return UIValue(variant.Get<bool>());
        case Rml::Variant::BYTE:
        case Rml::Variant::CHAR:
        case Rml::Variant::INT:
        case Rml::Variant::INT64:
        case Rml::Variant::UINT:
        case Rml::Variant::UINT64:
        case Rml::Variant::FLOAT:
        case Rml::Variant::DOUBLE:
            return UIValue(variant.Get<double>());
        default:
            return UIValue(variant.Get<Rml::String>());
    }
}

bool HtmlUIValueDefinition::Get(void* ptr, Rml::Variant& variant){
    toVariant(*static_cast<const UIValue*>(ptr), variant);
    return true;
}

bool HtmlUIValueDefinition::Set(void* ptr, const Rml::Variant& variant){
    UIValue& value = *static_cast<UIValue*>(ptr);

    // keep the type the model gave the value where the new one converts (an <input> writes
    // strings, a range or a number field included)
    UIValue written = fromVariant(variant);
    if (value.getType() == UIValue::Type::NUMBER && written.getType() == UIValue::Type::STRING){
        const std::string& text = written.asString();
        char* end = nullptr;
        const double number = std::strtod(text.c_str(), &end);
        if (end != text.c_str() && *end == '\0'){
            written = UIValue(number);
        }
    }else if (value.getType() == UIValue::Type::BOOL && written.getType() != UIValue::Type::BOOL){
        written = UIValue(written.asBool());
    }else if (value.getType() == UIValue::Type::STRING && written.getType() == UIValue::Type::NUMBER){
        Rml::String text;
        written = variant.GetInto(text) ? UIValue(text) : written;
    }

    if (value == written)
        return true;

    value = std::move(written);
    if (record){
        record->markWritten(topLevel);
    }
    return true;
}

int HtmlUIValueDefinition::Size(void* ptr){
    return (int)static_cast<const UIValue*>(ptr)->size();
}

Rml::DataVariable HtmlUIValueDefinition::Child(void* ptr, const Rml::DataAddressEntry& address){
    UIValue& value = *static_cast<UIValue*>(ptr);

    if (value.getType() == UIValue::Type::ARRAY){
        std::vector<UIValue>& items = HtmlUIScriptAccess::items(value);
        if (address.index >= 0 && (size_t)address.index < items.size())
            return Rml::DataVariable(this, &items[(size_t)address.index]);
        if (address.name == "size")
            return Rml::MakeLiteralIntVariable((int)items.size());
        return Rml::DataVariable();
    }

    if (value.getType() == UIValue::Type::OBJECT && !address.name.empty()){
        std::vector<UIValue>& items = HtmlUIScriptAccess::items(value);
        const std::vector<std::string>& keys = HtmlUIScriptAccess::objectKeys(value);
        for (size_t i = 0; i < keys.size(); i++){
            if (keys[i] == address.name)
                return Rml::DataVariable(this, &items[i]);
        }
    }

    return Rml::DataVariable();
}

Rml::StringList HtmlUIValueDefinition::ReflectMemberNames(){
    return Rml::StringList();
}
