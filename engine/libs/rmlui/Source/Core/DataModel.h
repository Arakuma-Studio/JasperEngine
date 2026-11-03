#pragma once

#include "../../Include/RmlUi/Core/DataModelHandle.h"
#include "../../Include/RmlUi/Core/DataTypes.h"
#include "../../Include/RmlUi/Core/Header.h"
#include "../../Include/RmlUi/Core/Traits.h"
#include "../../Include/RmlUi/Core/Types.h"

namespace Rml {

class DataViews;
class DataControllers;
class DataVariable;
class Element;
class FuncDefinition;

class DataModel : NonCopyMoveable {
public:
	DataModel(DataTypeRegister* data_type_register = nullptr, bool allow_missing_variables = false);
	~DataModel();

	void AddView(DataViewPtr view);
	void AddController(DataControllerPtr controller);

	bool BindVariable(const String& name, DataVariable variable);
	bool BindFunc(const String& name, DataGetFunc get_func, DataSetFunc set_func);

	bool BindEventCallback(const String& name, DataEventFunc event_func);

	bool InsertAlias(Element* element, const String& alias_name, DataAddress replace_with_address);
	bool EraseAliases(Element* element);
	void CopyAliases(Element* source_element, Element* target_element);

	// Fork (P7-D): rebindable row aliases, for the pooled rows of the engine's virtual views. A row alias is an alias of
	// `row` whose address is looked up when it is read: the views under the row resolve it to [first name of target,
	// row entry] once, and every evaluation reads the row's current target. SetRowAlias re-points it and updates only
	// the views in the row's subtree (at the next Update). Returns the alias's handle, -1 when the target is invalid.
	int InsertRowAlias(Element* row, const String& alias_name, DataAddress target);
	// Re-points a row alias (its handle from InsertRowAlias); the row's views are updated when the target changed.
	void SetRowAlias(int handle, DataAddress target);
	// The current target of a row alias (empty for an unknown handle).
	const DataAddress& GetRowAlias(int handle) const;
	// The entry naming a row alias inside a resolved address: its name.
	static constexpr const char* RowAliasEntryName = "#row";

	DataAddress ResolveAddress(const String& address_str, Element* element) const;
	const DataEventFunc* GetEventCallback(const String& name);

	DataVariable GetVariable(const DataAddress& address) const;
	bool GetVariableInto(const DataAddress& address, Variant& out_value) const;

	void DirtyVariable(const String& variable_name);
	bool IsVariableDirty(const String& variable_name) const;
	void DirtyAllVariables();

	bool CallTransform(const String& name, const VariantList& arguments, Variant& out_result) const;

	// Elements declaring 'data-model' need to be attached.
	void AttachModelRootElement(Element* element);
	ElementList GetAttachedModelRootElements() const;

	void OnElementRemove(Element* element);

	bool Update(bool clear_dirty_variables);

	DataTypeRegister* GetDataTypeRegister() const { return data_type_register; }
	bool AllowMissingVariables() const { return allow_missing_variables; }
	const UnorderedMap<String, DataVariable>& GetAllVariables() const { return variables; }

private:
	UniquePtr<DataViews> views;
	UniquePtr<DataControllers> controllers;

	UnorderedMap<String, DataVariable> variables;
	DirtyVariables dirty_variables;

	UnorderedMap<String, UniquePtr<FuncDefinition>> function_variable_definitions;
	UnorderedMap<String, DataEventFunc> event_callbacks;

	using ScopedAliases = UnorderedMap<Element*, SmallUnorderedMap<String, DataAddress>>;
	ScopedAliases aliases;

	// Fork (P7-D): the row aliases by handle (row null: a free handle), the handles of each row, the rows re-pointed
	// since the last Update.
	struct RowAlias {
		Element* row = nullptr;
		DataAddress target;
	};
	Vector<RowAlias> row_aliases;
	Vector<int> free_row_aliases;
	UnorderedMap<Element*, Vector<int>> row_alias_handles;
	SmallUnorderedSet<Element*> dirty_rows;

	DataVariable GetVariable(const DataAddress& head, const DataAddress& address, size_t address_begin) const;

	DataTypeRegister* data_type_register;
	bool allow_missing_variables;

	SmallUnorderedSet<Element*> attached_elements;
};

} // namespace Rml
