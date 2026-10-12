#pragma once

#include "rhythm/Chart.h"

#include "core/Result.h"

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// yyjson's own type names; the library stays a private dependency.
struct yyjson_val;     // NOLINT(readability-identifier-naming)
struct yyjson_mut_doc; // NOLINT(readability-identifier-naming)
struct yyjson_mut_val; // NOLINT(readability-identifier-naming)

namespace hyoshi::rhythm
{

// Read-only access to one JSON object of a chart file, so modes parse their own fields without
// depending on the JSON library. Errors name the field.
class ChartFields
{
public:
    explicit ChartFields(const yyjson_val* object) : object(object)
    {
    }

    bool Has(std::string_view key) const;

    Result<int64_t> GetInt(std::string_view key) const;
    Result<double> GetNumber(std::string_view key) const;
    Result<std::string> GetString(std::string_view key) const;
    Result<bool> GetBool(std::string_view key) const;

    // Return the fallback when the field is missing, and an error when it has the wrong type.
    Result<int64_t> GetInt(std::string_view key, int64_t fallback) const;
    Result<double> GetNumber(std::string_view key, double fallback) const;
    Result<std::string> GetString(std::string_view key, std::string fallback) const;
    Result<bool> GetBool(std::string_view key, bool fallback) const;

    // Whether the field is an array (of keys, say, rather than one number).
    bool IsArray(std::string_view key) const;
    // Calls fn(element, index) for each object of the array field, with errors naming the element.
    // A missing field is an empty array.
    Result<void> ForEachObject(std::string_view key,
                               const std::function<Result<void>(const ChartFields&, size_t)>& fn) const;
    // The same, but a null field is an empty array and null elements are skipped, as some formats
    // write them for what's unused.
    Result<void> ForEachPresentObject(std::string_view key,
                                      const std::function<Result<void>(const ChartFields&, size_t)>& fn) const;
    // An array of numbers, such as [1, 2, 4].
    Result<std::vector<double>> GetNumbers(std::string_view key) const;
    // An array of strings.
    Result<std::vector<std::string>> GetStrings(std::string_view key) const;
    // A nested object.
    Result<ChartFields> GetObject(std::string_view key) const;

private:
    const yyjson_val* Find(std::string_view key) const;

    const yyjson_val* object;
};

// Adds fields to one JSON object of a chart being written.
class ChartFieldWriter
{
public:
    ChartFieldWriter(yyjson_mut_doc* document, yyjson_mut_val* object) : document(document), object(object)
    {
    }

    void AddInt(std::string_view key, int64_t value);
    void AddNumber(std::string_view key, double value);
    void AddString(std::string_view key, std::string_view value);
    void AddBool(std::string_view key, bool value);
    // A nested object.
    ChartFieldWriter AddObject(std::string_view key);
    // An array of `count` objects, each filled by fn(element, index).
    void AddObjects(std::string_view key, size_t count, const std::function<void(ChartFieldWriter&, size_t)>& fn);

private:
    yyjson_mut_doc* document;
    yyjson_mut_val* object;
};

// A mode's part of the file format: its modeSettings object and its note payload fields.
struct ChartModeFormat
{
    std::string Mode;
    std::function<Result<void>(const ChartFields& settings)> ReadSettings;
    // Called for every note, in file order, with the note's index.
    std::function<Result<void>(const ChartFields& note, size_t index)> ReadNote;
    std::function<void(ChartFieldWriter& settings)> WriteSettings;
    std::function<void(ChartFieldWriter& note, size_t index)> WriteNote;
};

// Parses and validates a .rchart.json file (DESIGN.md section 13.4): the format version, the
// mode, sorted notes and timing, and end times not before start times.
Result<Chart> ParseChart(std::string_view json, const ChartModeFormat& mode);

// The same for a chart of any of `modes`: format 1 (one "mode"), or format 2 (DESIGN.md section
// 13.5: "modes" lists the chart's modes, "modeSettings" holds an object per mode, and each note
// says its "mode" when there's more than one). Each note's own mode reads its fields, so a mode's
// ReadNote sees only its notes, in order. A mode the chart has but `modes` doesn't is an error.
Result<Chart> ParseChart(std::string_view json, std::span<const ChartModeFormat> modes);

// Parses a JSON document whose root is an object and reads it with `read`: for importers of other
// formats, which stay free of the JSON library.
Result<void> ReadJson(std::string_view json, const std::function<Result<void>(const ChartFields& root)>& read);

// Writes a .rchart.json file, indented for readable diffs.
Result<std::string> WriteChart(const Chart& chart, const ChartModeFormat& mode);

// Writes a chart of any of `modes` as format 2 (DESIGN.md section 13.5): "modes" as chart.Modes
// lists them, each one's settings under its name, and each note's "mode" (its ChartNote::Mode). A
// mode's WriteNote gets the note's place among that mode's notes, as its ReadNote saw them. Each
// of the chart's modes needs its format in `modes`.
Result<std::string> WriteChart(const Chart& chart, std::span<const ChartModeFormat> modes);

} // namespace hyoshi::rhythm
