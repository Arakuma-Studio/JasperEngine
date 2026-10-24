// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "mcp/McpJson.h"

#include <functional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

// Command-line arguments of an MCP tool, derived from its JSON Schema (`jasperengine-editor call`,
// docs/mcp/cli.md): how each property becomes a flag, the typed conversion of flag values, and a
// validator for the schema subset the McpSchema builders emit (docs/mcp/conventions.md). Pure
// functions: no editor, no I/O except the file reader given for @path values.
namespace jasperengine::editor::cli {

    using Json = mcp::Json;

    // The exit codes of call / batch / list-tools / editors
    enum Exit { Ok = 0, ToolError = 1, Usage = 2, Connection = 3, Internal = 4 };

    // A bad flag, value or argument shape (exit code 2)
    class UsageError : public std::runtime_error {
    public:
        explicit UsageError(const std::string& message): std::runtime_error(message) {}
    };

    // How a property's values are written on the command line
    enum class ArgKind {
        String,       // verbatim
        Integer,      // strict decimal
        Number,       // a finite float
        Boolean,      // --k, --k=V / --k V (true/false/1/0/yes/no/on/off), --no-k
        Enum,         // one of the schema's values
        Ref,          // type [integer, string]: digits = an id, "\"42\"" = the name 42, else a string
        ScalarArray,  // a,b,c, a JSON array, or the flag repeated
        FreeObject,   // JSON, or K=V repeated (V is JSON when it parses, else a string)
        Json          // JSON (objects with declared keys, arrays of objects, anyOf, any)
    };

    struct ArgSpec {
        std::string key;    // the property name (snake_case)
        std::string flag;   // --<key with - for _>
        ArgKind kind = ArgKind::Json;
        bool required = false;
        Json schema;        // the property schema
    };

    // The flags of a tool's inputSchema, in its property order
    std::vector<ArgSpec> argSpecs(const Json& inputSchema);
    ArgKind classify(const Json& propertySchema);

    // Reads the contents of an @path value ("-" = standard input); throws UsageError when it can't
    using FileReader = std::function<std::string(const std::string& path)>;
    // The default reader: files relative to the working directory, "-" from std::cin
    std::string readArgFile(const std::string& path);

    // The tool arguments of `call <tool> [tool-args]`: one positional JSON object ('{...}',
    // @file.json, @- for stdin) or flags, never both. Throws UsageError (unknown flag with a
    // did-you-mean and the valid flags, bad value, mixed forms). Does not check required or
    // ranges: validate() does.
    Json parseToolArgs(const Json& inputSchema, std::span<const std::string> tokens, const FileReader& reader = readArgFile);

    // Every way value breaks schema (the builders' subset: type, enum, required, properties,
    // additionalProperties, items, minItems, maxItems, minimum, maximum, anyOf), as
    // "<path>: <problem>" lines; empty when it is valid. The top level of a tool's inputSchema
    // is closed even when the listing leaves additionalProperties out (the registry enforces it):
    // pass closedTop.
    std::vector<std::string> validate(const Json& schema, const Json& value, bool closedTop = false);

    // A short type text for help: "integer 1..100", "vec3 number[3]", "one of: a | b", ...
    std::string describeType(const Json& propertySchema);

    // Levenshtein distance (did-you-mean suggestions)
    size_t editDistance(const std::string& a, const std::string& b);
    // The candidates within distance 2 of word, closest first
    std::vector<std::string> closeMatches(const std::string& word, const std::vector<std::string>& candidates);

    // snake_case or kebab-case to the snake_case key ("create-entity" -> "create_entity")
    std::string snakeName(const std::string& name);

}
