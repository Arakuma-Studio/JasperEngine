# LuaBridge3: local patches

`LuaBridge.h` is the amalgamated header of [LuaBridge3](https://github.com/kunitoki/LuaBridge3),
last updated from upstream master in `c338dffd8`. Every local change is listed here and marked in
the header by a `// JasperEngine (...)` comment. An upgrade takes the new upstream header and
applies each change again, in this order, then runs the tests named with it.

Lua is compiled as C (`engine/libs/lua/CMakeLists.txt`), so `lua_error` is a `longjmp`: it runs no
C++ destructor and ends no `catch` handler. Several changes exist for that reason.

## 1. S34 D6: failure paths out of line, registration cold (`2a032453b`)

`docs/specs/S34-player-size.md` D6, for the size of the player.

- `LUABRIDGE_COLD` and `LUABRIDGE_COLD_NOINLINE` (Config section).
- Registration (`Namespace`, `Class` and their `add*`, `begin*`, `end*` members) is cold, so it is
  optimised for size.
- The thunks' failures are raised from one function each: `raise_error_code`,
  `raise_argument_error`, and the exception helper (since S92: change 4). Each puts its message on
  the Lua stack before formatting, so no `std::string` is alive when the error leaves.
- `tostring_metamethod` is one function for every class (it never used its class parameter).
- Messages and behaviour unchanged.
- Test: `LuaBindingBench.CallsAndProperties` (`engine/tests/core/script/LuaBindingBenchTest.cpp`)
  compares builds; the binding tests under `engine/tests/core/script/`.

## 2. The exception helper calls `raise_lua_error` (`583652b17`)

S34's helper called itself (unbounded recursion: a stack overflow instead of a Lua error when a
binding threw). Fixed to format `e.what()`.

- Test: `Physics2DReview.LuaArgumentErrorsAreCatchableErrors` (editor tests),
  `tools/mcp/tests/test_shader_globals.py::test_library_parameter_bound_to_a_global`.

## 3. S92 R24, R25: overload dispatch without a pcall or a string per miss

`docs/specs/S92-htmlui-render-cost.md` R24, R25, D12. The JasperArcade port's patch, taken as is.

- Each overload entry gets a third field: a no-raise check of its arguments
  (`overload_arg_may_decode`), built from what `Stack<T>::get` accepts:
  - numbers and `const char*`: `Stack<T>::get` itself (it does not raise);
  - `std::string`: a string or a number;
  - a bound class (pointer, reference or value): `Userdata::isInstance` or nil;
  - anything else (`bool`, enums, `LuaRef`, containers, `std::optional`, `std::string_view`,
    the engine's own `Stack` specialisations): "can't tell", so the overload runs.
  - `lua_CFunction` overloads and `addConstructorFrom` have no check.
- `try_overload_functions` skips an overload whose check says no, without calling it.
- No error table and no string is made until every overload failed. Then a second pass builds the
  message stock builds: same order, same `#%d` numbering, the stored errors of the bodies that ran,
  and each skipped overload run again for its own error text.
- Test: `LuaOverloadDispatch.CallsMatchStockLuaBridge` (every result and message against
  `engine/tests/core/fixtures/luabridge/overload-dispatch.txt`, captured from the stock header),
  `LuaOverloadDispatch.LaterOverloadBuildsNoErrorString`, `LuaBindingBench.OverloadMisses`.

## 4. S92 R26: no C++ object left behind by a Lua error

`docs/specs/S92-htmlui-render-cost.md` R26. Every path below used to `longjmp` over live C++
objects (the decoded `std::string` of an argument, a half-built container, a caught exception):

- **Argument decoding** (`make_arguments_list`). A pack whose decoded values are all trivially
  destructible keeps the decoder that raises in place. Any other pack decodes through
  `decode_arguments` into `std::optional`s, inside an `ArgumentDecodeScope`, and a failure is raised
  by `raise_argument_decode_failure` after they are destroyed. Same evaluation order as before (the
  compiler's order of a call's arguments), so the argument named is the one stock names.
- **A bound class that does not match** (`Userdata::getClass`). Inside a decode scope,
  `badArgument` records `luaL_argerror`'s index and names instead of raising (also for an element
  inside a container); the decoder raises the same `luaL_argerror` afterwards. The scope belongs to
  the frame that opened it: a light userdata token, never reused, sits in that frame's first slot
  past its arguments. A call nested in a decode (a `__gc` run by an allocation) still raises in
  place, and a record left by a `longjmp` out of a decode (a memory error) never matches again. The
  arguments past the stack top are read one slot higher, so they still read as "none".
- **Property setters** decode their value the same way (`decode_property_value`; the error keeps
  the setter's own text).
- **Exceptions** (`push_exception_message`, `raise_pushed_error`). The handler only pushes
  `e.what()`; the error is raised after the handler ended. A `longjmp` out of a `catch` block left
  the exception object alive and the runtime's caught-exception stack holding it
  (`std::current_exception()` stayed set).
- **Constructors**: the decoded arguments and the container of `container_forwarder` live in a
  block of their own; every error is raised after it. `ec.message().c_str()` temporaries became
  `raise_error_code`.
- **`read_only_error`** formats with Lua instead of a `std::stringstream`.
- Messages unchanged.
- Test: `LuaOverloadDispatch.NoLeakOnMiss`, `.NoLeakWhenEveryOverloadFails`,
  `.NoLeakOnWrongCallWithoutOverloads`, `.NoLeakWhenABodyThrows`, and the message test of change 3.

## 5. S92 R27: debug guards of the overload checks

`docs/specs/S92-htmlui-render-cost.md` R27.

- Where `LUABRIDGE_ASSERT` is active (no `NDEBUG`, or `LUABRIDGE_FORCE_ASSERT_RELEASE`), a check
  that says no also decodes the arguments (without raising) and asserts that the decode fails.
- The second pass of `try_overload_functions` asserts that no overload it runs succeeds. In a
  release build such a call still runs rather than being dropped.
- Test: the `LuaOverloadDispatch` tests in a build without `NDEBUG`.

## 6. S92, S95 REQ-2: nil and class metatables are not objects

Two values were read as an object and crashed the process (found by S95 WP-1):

- **nil as the object of a member call** (`Vector3.length(nil)`, `nil + v`, `nil < v`, an
  accessor reached through `debug.getmetatable`). `Userdata::get` returns a null pointer for nil,
  as an argument's nil means; the member thunks dereferenced it. They now take their object
  through `Userdata::getSelf`, which raises for nil the bad-self error any other value raises
  (`bad argument #1 to 'length' (Vector3 expected, got nil)`): member functions, const ones,
  C-function members, property getters and setters. A function or functor bound as a member with
  a pointer as its first parameter (`invoke_proxy_member_function`, `invoke_proxy_member_functor`,
  used by `push_member_function` and the class property helpers) refuses a nil object the same
  way; a reference parameter refused it already. Free and static functions keep nil as a null
  pointer.
- **A class's metatable as an object** (`debug.getmetatable(v)`: a table whose own metatable is
  itself). `getClass` matched it and used its null userdata pointer. It now matches only a full
  userdata; `Userdata::isInstance` answers false for anything else. The error names the type
  ("got table"), with the same expected name as before.
- Messages of every other call unchanged; the 22 nil-self calls of the dispatch fixture changed
  from a call on a null object to the bad-self error (deliberate, in the same commit).
- Test: `LuaBridgeSelf.*` (`engine/tests/core/script/LuaBridgeSelfTest.cpp`; each test crashed the
  process on the header before this change), `LuaOverloadDispatch.CallsMatchStockLuaBridge`.

