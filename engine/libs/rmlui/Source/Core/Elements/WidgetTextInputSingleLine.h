#pragma once

#include "WidgetTextInput.h"

namespace Rml {

/**
    A specialisation of the text input widget for single-line input fields.
 */

class WidgetTextInputSingleLine : public WidgetTextInput {
public:
	WidgetTextInputSingleLine(ElementFormControl* parent);

protected:
	/// Removes any invalid characters from the string.
	void SanitizeValue(String& value) override;
	/// Called when the user pressed enter.
	void LineBreak() override;
	/// Fork (P4-G3): the line is centered in the content box.
	float GetTextOffsetTop() const override;
};

} // namespace Rml
