#pragma once

#include "rhythm/Chart.h"

#include "core/Result.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

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

// Writes a .rchart.json file, indented for readable diffs.
Result<std::string> WriteChart(const Chart& chart, const ChartModeFormat& mode);

} // namespace hyoshi::rhythm
