# Local changes to Dear ImGui

Dear ImGui 1.92.8 is vendored unmodified except for the changes below. Re-apply them after an
upgrade (search for `[JasperEngine patch]`); `ImGuiNumericHook.*` in `jasperengine-editor-tests`
fails loudly when one is missing.

## Expressions in numeric fields (S23 R26)

So every `Drag*`, `Slider*` (Ctrl+click) and `Input*` scalar field accepts math expressions
(`editor/util/NumericExpression`) without changing any call site:

- `imgui_internal.h`: `ImGui::DataTypeApplyFromTextHook` and `ImGui::SetDataTypeApplyFromTextHook(hook, user_data)`.
- `imgui_widgets.cpp`, `DataTypeApplyFromText()`: after the empty-text check, the hook is called
  first; a result `>= 0` is used as is (1: the value changed), `< 0` falls through to the stock
  `sscanf` parsing (the hook returns that for plain numbers).
- `imgui_widgets.cpp`, `TempInputScalar()`: the text buffer is 256 bytes (was 32), and
  `InputScalar()`: 256 bytes (was 64), so a long expression is not cut.
- `imgui_widgets.cpp`, `InputTextFilterCharacter()`: with the hook installed, `,` is no longer
  turned into the decimal point in numeric fields (it separates arguments: `clamp(x, 0, 1)`).

## Inline IME composition on macOS (S82 R37)

So the text an input method is composing (げきあつ before it becomes 激熱) can be drawn inline
(`editor/util/ImePreedit.h`; the Title Designer's canvas text editor draws it), `backends/imgui_impl_osx.mm`:

- includes `util/ImePreedit.h` (the editor's include root);
- `KeyEventResponder`: `hasMarkedText`, `markedRange`, `selectedRange`, `setMarkedText` and
  `unmarkText` are backed by `ImePreedit` (two `NSRange` ivars keep the UTF-16 ranges);
  `insertText` clears it before the committed text goes to `io.AddInputCharactersUTF8` as before;
- `Platform_SetImeDataFn`: clears it when no field wants text (after `discardMarkedText`);
- `ImGui_ImplOSX_HandleEvent()`: a key press while composing returns as used without reaching
  ImGui (the input method owns Enter, Esc, arrows and Backspace then); releases still pass.

Drop this patch if upstream ImGui gains marked-text support.
