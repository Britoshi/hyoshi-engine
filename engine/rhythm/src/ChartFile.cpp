#include "rhythm/ChartFile.h"

#include <yyjson.h>

#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string>
#include <utility>

namespace hyoshi::rhythm
{

namespace
{

struct DocumentDeleter
{
    void operator()(yyjson_doc* document) const
    {
        yyjson_doc_free(document);
    }

    void operator()(yyjson_mut_doc* document) const
    {
        yyjson_mut_doc_free(document);
    }
};

std::string Quote(std::string_view key)
{
    return "'" + std::string(key) + "'";
}

Error WithContext(const std::string& context, const Error& error)
{
    return Error{context + ": " + error.Message};
}

yyjson_val* FindMember(const yyjson_val* object, std::string_view key)
{
    return yyjson_obj_getn(const_cast<yyjson_val*>(object), key.data(), key.size());
}

Result<const yyjson_val*> GetObject(const yyjson_val* parent, std::string_view key)
{
    const yyjson_val* value = FindMember(parent, key);
    if (value == nullptr)
    {
        return Error{"missing " + Quote(key)};
    }
    if (!yyjson_is_obj(const_cast<yyjson_val*>(value)))
    {
        return Error{Quote(key) + " must be an object"};
    }
    return value;
}

// Missing arrays read as empty, which a null value stands for.
Result<const yyjson_val*> GetOptionalArray(const yyjson_val* parent, std::string_view key)
{
    const yyjson_val* value = FindMember(parent, key);
    if (value != nullptr && !yyjson_is_arr(const_cast<yyjson_val*>(value)))
    {
        return Error{Quote(key) + " must be an array"};
    }
    return value;
}

// Calls fn(fields, element, index) for every element of an array of objects.
template <typename Fn>
Result<void> ForEachObject(const yyjson_val* array, std::string_view name, Fn&& fn)
{
    if (array == nullptr)
    {
        return {};
    }

    size_t index = 0;
    size_t count = 0;
    yyjson_val* element = nullptr;
    yyjson_arr_foreach(const_cast<yyjson_val*>(array), index, count, element)
    {
        const std::string context = std::string(name) + "[" + std::to_string(index) + "]";
        if (!yyjson_is_obj(element))
        {
            return Error{context + " must be an object"};
        }
        if (Result<void> result = fn(ChartFields(element), element, index); !result)
        {
            return WithContext(context, result.GetError());
        }
    }
    return {};
}

// Moves a successful result into `target`.
template <typename T>
Result<void> Assign(Result<T> result, T& target)
{
    if (!result)
    {
        return result.GetError();
    }
    target = std::move(result).Value();
    return {};
}

Result<void> ReadMetadata(const ChartFields& fields, ChartMetadata& metadata)
{
    const Result<void> results[] = {
        Assign(fields.GetString("title", ""), metadata.Title),
        Assign(fields.GetString("artist", ""), metadata.Artist),
        Assign(fields.GetString("charter", ""), metadata.Charter),
        Assign(fields.GetString("difficultyName", ""), metadata.DifficultyName),
        Assign(fields.GetNumber("difficultyValue", 0.0), metadata.DifficultyValue),
        Assign(fields.GetInt("previewStartUs", 0), metadata.PreviewStartUs),
    };
    for (const Result<void>& result : results)
    {
        if (!result)
        {
            return result;
        }
    }
    return {};
}

Result<void> ReadMeter(const yyjson_val* point, TimingPoint& timing)
{
    const yyjson_val* meter = FindMember(point, "meter");
    if (meter == nullptr)
    {
        return {};
    }

    yyjson_val* beats = yyjson_arr_get(const_cast<yyjson_val*>(meter), 0);
    yyjson_val* unit = yyjson_arr_get(const_cast<yyjson_val*>(meter), 1);
    if (!yyjson_is_arr(const_cast<yyjson_val*>(meter)) || yyjson_arr_size(const_cast<yyjson_val*>(meter)) != 2 ||
        !yyjson_is_uint(beats) || !yyjson_is_uint(unit) || yyjson_get_uint(beats) == 0 || yyjson_get_uint(unit) == 0 ||
        yyjson_get_uint(beats) > 64 || yyjson_get_uint(unit) > 64)
    {
        return Error{"'meter' must be two positive integers, like [4, 4]"};
    }
    timing.BeatsPerBar = static_cast<uint32_t>(yyjson_get_uint(beats));
    timing.BeatUnit = static_cast<uint32_t>(yyjson_get_uint(unit));
    return {};
}

// yyjson copies the key, so writers can pass temporary strings.
void AddMember(yyjson_mut_doc* document, yyjson_mut_val* object, std::string_view key, yyjson_mut_val* value)
{
    yyjson_mut_obj_add(object, yyjson_mut_strncpy(document, key.data(), key.size()), value);
}

} // namespace

bool ChartFields::Has(std::string_view key) const
{
    return Find(key) != nullptr;
}

const yyjson_val* ChartFields::Find(std::string_view key) const
{
    return FindMember(object, key);
}

Result<int64_t> ChartFields::GetInt(std::string_view key) const
{
    yyjson_val* value = const_cast<yyjson_val*>(Find(key));
    if (value == nullptr)
    {
        return Error{"missing " + Quote(key)};
    }
    if (yyjson_is_sint(value))
    {
        return yyjson_get_sint(value);
    }
    if (yyjson_is_uint(value) && yyjson_get_uint(value) <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
    {
        return static_cast<int64_t>(yyjson_get_uint(value));
    }
    return Error{Quote(key) + " must be an integer"};
}

Result<double> ChartFields::GetNumber(std::string_view key) const
{
    yyjson_val* value = const_cast<yyjson_val*>(Find(key));
    if (value == nullptr)
    {
        return Error{"missing " + Quote(key)};
    }
    if (!yyjson_is_num(value) || !std::isfinite(yyjson_get_num(value)))
    {
        return Error{Quote(key) + " must be a number"};
    }
    return yyjson_get_num(value);
}

Result<std::string> ChartFields::GetString(std::string_view key) const
{
    yyjson_val* value = const_cast<yyjson_val*>(Find(key));
    if (value == nullptr)
    {
        return Error{"missing " + Quote(key)};
    }
    if (!yyjson_is_str(value))
    {
        return Error{Quote(key) + " must be a string"};
    }
    return std::string(yyjson_get_str(value), yyjson_get_len(value));
}

Result<bool> ChartFields::GetBool(std::string_view key) const
{
    yyjson_val* value = const_cast<yyjson_val*>(Find(key));
    if (value == nullptr)
    {
        return Error{"missing " + Quote(key)};
    }
    if (!yyjson_is_bool(value))
    {
        return Error{Quote(key) + " must be true or false"};
    }
    return yyjson_get_bool(value);
}

Result<int64_t> ChartFields::GetInt(std::string_view key, int64_t fallback) const
{
    return Has(key) ? GetInt(key) : Result<int64_t>(fallback);
}

Result<double> ChartFields::GetNumber(std::string_view key, double fallback) const
{
    return Has(key) ? GetNumber(key) : Result<double>(fallback);
}

Result<std::string> ChartFields::GetString(std::string_view key, std::string fallback) const
{
    return Has(key) ? GetString(key) : Result<std::string>(std::move(fallback));
}

Result<bool> ChartFields::GetBool(std::string_view key, bool fallback) const
{
    return Has(key) ? GetBool(key) : Result<bool>(fallback);
}

void ChartFieldWriter::AddInt(std::string_view key, int64_t value)
{
    AddMember(document, object, key, yyjson_mut_sint(document, value));
}

void ChartFieldWriter::AddNumber(std::string_view key, double value)
{
    AddMember(document, object, key, yyjson_mut_real(document, value));
}

void ChartFieldWriter::AddString(std::string_view key, std::string_view value)
{
    AddMember(document, object, key, yyjson_mut_strncpy(document, value.data(), value.size()));
}

void ChartFieldWriter::AddBool(std::string_view key, bool value)
{
    AddMember(document, object, key, yyjson_mut_bool(document, value));
}

Result<Chart> ParseChart(std::string_view json, const ChartModeFormat& mode)
{
    yyjson_read_err readError{};
    const std::unique_ptr<yyjson_doc, DocumentDeleter> document(
        yyjson_read_opts(const_cast<char*>(json.data()), json.size(), YYJSON_READ_NOFLAG, nullptr, &readError));
    if (!document)
    {
        return Error{"Invalid JSON at byte " + std::to_string(readError.pos) + ": " + readError.msg};
    }

    const yyjson_val* root = yyjson_doc_get_root(document.get());
    if (!yyjson_is_obj(const_cast<yyjson_val*>(root)))
    {
        return Error{"A chart must be a JSON object"};
    }
    const ChartFields file(root);

    Result<int64_t> version = file.GetInt("formatVersion");
    if (!version)
    {
        return version.GetError();
    }
    if (version.Value() != Chart::FORMAT_VERSION)
    {
        return Error{"Unsupported chart format version " + std::to_string(version.Value()) + "; this build reads " +
                     std::to_string(Chart::FORMAT_VERSION)};
    }

    Chart chart;
    Result<std::string> chartMode = file.GetString("mode");
    if (!chartMode)
    {
        return chartMode.GetError();
    }
    if (chartMode.Value() != mode.Mode)
    {
        return Error{"This is a '" + chartMode.Value() + "' chart, not '" + mode.Mode + "'"};
    }
    chart.Mode = std::move(chartMode).Value();

    Result<const yyjson_val*> metadata = GetObject(root, "metadata");
    if (!metadata)
    {
        return metadata.GetError();
    }
    if (Result<void> result = ReadMetadata(ChartFields(metadata.Value()), chart.Metadata); !result)
    {
        return WithContext("metadata", result.GetError());
    }

    Result<const yyjson_val*> audio = GetObject(root, "audio");
    if (!audio)
    {
        return audio.GetError();
    }
    const ChartFields audioFields(audio.Value());
    Result<std::string> audioFile = audioFields.GetString("file");
    Result<int64_t> audioOffset = audioFields.GetInt("offsetUs", 0);
    if (!audioFile || !audioOffset)
    {
        return WithContext("audio", audioFile ? audioOffset.GetError() : audioFile.GetError());
    }
    chart.AudioFile = std::move(audioFile).Value();
    chart.AudioOffsetUs = audioOffset.Value();

    Result<const yyjson_val*> samples = GetOptionalArray(root, "samples");
    if (!samples)
    {
        return samples.GetError();
    }
    if (samples.Value() != nullptr)
    {
        size_t index = 0;
        size_t count = 0;
        yyjson_val* element = nullptr;
        yyjson_arr_foreach(const_cast<yyjson_val*>(samples.Value()), index, count, element)
        {
            if (!yyjson_is_str(element))
            {
                return Error{"samples[" + std::to_string(index) + "] must be a string"};
            }
            chart.Samples.emplace_back(yyjson_get_str(element), yyjson_get_len(element));
        }
    }

    Result<const yyjson_val*> timing = GetOptionalArray(root, "timing");
    if (!timing)
    {
        return timing.GetError();
    }
    Result<void> timingResult =
        ForEachObject(timing.Value(), "timing",
                      [&chart](const ChartFields& fields, const yyjson_val* element, size_t) -> Result<void>
                      {
                          TimingPoint point;
                          if (Result<void> result = Assign(fields.GetInt("timeUs"), point.Time); !result)
                          {
                              return result;
                          }
                          if (Result<void> result = Assign(fields.GetNumber("bpm"), point.Bpm); !result)
                          {
                              return result;
                          }
                          if (point.Bpm <= 0.0)
                          {
                              return Error{"'bpm' must be positive"};
                          }
                          if (!chart.Timing.empty() && point.Time < chart.Timing.back().Time)
                          {
                              return Error{"timing points must be sorted by time"};
                          }
                          if (Result<void> result = ReadMeter(element, point); !result)
                          {
                              return result;
                          }
                          chart.Timing.push_back(point);
                          return {};
                      });
    if (!timingResult)
    {
        return timingResult.GetError();
    }

    Result<const yyjson_val*> scrollVelocity = GetOptionalArray(root, "scrollVelocity");
    if (!scrollVelocity)
    {
        return scrollVelocity.GetError();
    }
    Result<void> scrollResult =
        ForEachObject(scrollVelocity.Value(), "scrollVelocity",
                      [&chart](const ChartFields& fields, const yyjson_val*, size_t) -> Result<void>
                      {
                          Result<int64_t> time = fields.GetInt("timeUs");
                          Result<double> multiplier = fields.GetNumber("multiplier");
                          if (!time || !multiplier)
                          {
                              return time ? multiplier.GetError() : time.GetError();
                          }
                          if (!chart.ScrollVelocity.empty() && time.Value() < chart.ScrollVelocity.back().Time)
                          {
                              return Error{"scroll velocity points must be sorted by time"};
                          }
                          chart.ScrollVelocity.push_back({time.Value(), multiplier.Value()});
                          return {};
                      });
    if (!scrollResult)
    {
        return scrollResult.GetError();
    }

    if (mode.ReadSettings)
    {
        Result<const yyjson_val*> settings = GetObject(root, "modeSettings");
        if (!settings)
        {
            return settings.GetError();
        }
        if (Result<void> result = mode.ReadSettings(ChartFields(settings.Value())); !result)
        {
            return WithContext("modeSettings", result.GetError());
        }
    }

    const yyjson_val* notes = FindMember(root, "notes");
    if (notes == nullptr || !yyjson_is_arr(const_cast<yyjson_val*>(notes)))
    {
        return Error{"'notes' must be an array"};
    }
    chart.Notes.reserve(yyjson_arr_size(const_cast<yyjson_val*>(notes)));
    Result<void> notesResult =
        ForEachObject(notes, "notes",
                      [&chart, &mode](const ChartFields& fields, const yyjson_val*, size_t index) -> Result<void>
                      {
                          Result<int64_t> time = fields.GetInt("timeUs");
                          if (!time)
                          {
                              return time.GetError();
                          }
                          Result<int64_t> endTime = fields.GetInt("endTimeUs", time.Value());
                          if (!endTime)
                          {
                              return endTime.GetError();
                          }
                          if (endTime.Value() < time.Value())
                          {
                              return Error{"'endTimeUs' is before 'timeUs'"};
                          }
                          if (!chart.Notes.empty() && time.Value() < chart.Notes.back().Time)
                          {
                              return Error{"notes must be sorted by time"};
                          }
                          Result<int64_t> sample = fields.GetInt("sample", ChartNote::NO_SAMPLE);
                          Result<int64_t> sampleVolume = fields.GetInt("sampleVolume", 100);
                          if (!sample || !sampleVolume)
                          {
                              return sample ? sampleVolume.GetError() : sample.GetError();
                          }
                          if (sample.Value() != ChartNote::NO_SAMPLE &&
                              (sample.Value() < 0 || std::cmp_greater_equal(sample.Value(), chart.Samples.size())))
                          {
                              return Error{"'sample' " + std::to_string(sample.Value()) + " is not in 'samples'"};
                          }
                          if (sampleVolume.Value() < 0 || sampleVolume.Value() > 100)
                          {
                              return Error{"'sampleVolume' must be 0 to 100"};
                          }
                          chart.Notes.push_back({time.Value(), endTime.Value(), static_cast<int32_t>(sample.Value()),
                                                 static_cast<uint32_t>(sampleVolume.Value())});
                          return mode.ReadNote ? mode.ReadNote(fields, index) : Result<void>();
                      });
    if (!notesResult)
    {
        return notesResult.GetError();
    }

    return chart;
}

Result<std::string> WriteChart(const Chart& chart, const ChartModeFormat& mode)
{
    const std::unique_ptr<yyjson_mut_doc, DocumentDeleter> document(yyjson_mut_doc_new(nullptr));
    yyjson_mut_doc* doc = document.get();
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_sint(doc, root, "formatVersion", Chart::FORMAT_VERSION);
    yyjson_mut_obj_add_strncpy(doc, root, "mode", chart.Mode.data(), chart.Mode.size());

    ChartFieldWriter metadata(doc, yyjson_mut_obj_add_obj(doc, root, "metadata"));
    metadata.AddString("title", chart.Metadata.Title);
    metadata.AddString("artist", chart.Metadata.Artist);
    metadata.AddString("charter", chart.Metadata.Charter);
    metadata.AddString("difficultyName", chart.Metadata.DifficultyName);
    metadata.AddNumber("difficultyValue", chart.Metadata.DifficultyValue);
    metadata.AddInt("previewStartUs", chart.Metadata.PreviewStartUs);

    ChartFieldWriter audio(doc, yyjson_mut_obj_add_obj(doc, root, "audio"));
    audio.AddString("file", chart.AudioFile);
    audio.AddInt("offsetUs", chart.AudioOffsetUs);

    if (!chart.Samples.empty())
    {
        yyjson_mut_val* samples = yyjson_mut_obj_add_arr(doc, root, "samples");
        for (const std::string& sample : chart.Samples)
        {
            yyjson_mut_arr_add_strncpy(doc, samples, sample.data(), sample.size());
        }
    }

    yyjson_mut_val* timing = yyjson_mut_obj_add_arr(doc, root, "timing");
    for (const TimingPoint& point : chart.Timing)
    {
        yyjson_mut_val* object = yyjson_mut_arr_add_obj(doc, timing);
        yyjson_mut_obj_add_sint(doc, object, "timeUs", point.Time);
        yyjson_mut_obj_add_real(doc, object, "bpm", point.Bpm);
        yyjson_mut_val* meter = yyjson_mut_obj_add_arr(doc, object, "meter");
        yyjson_mut_arr_add_uint(doc, meter, point.BeatsPerBar);
        yyjson_mut_arr_add_uint(doc, meter, point.BeatUnit);
    }

    yyjson_mut_val* scrollVelocity = yyjson_mut_obj_add_arr(doc, root, "scrollVelocity");
    for (const ScrollVelocityPoint& point : chart.ScrollVelocity)
    {
        yyjson_mut_val* object = yyjson_mut_arr_add_obj(doc, scrollVelocity);
        yyjson_mut_obj_add_sint(doc, object, "timeUs", point.Time);
        yyjson_mut_obj_add_real(doc, object, "multiplier", point.Multiplier);
    }

    if (mode.WriteSettings)
    {
        ChartFieldWriter settings(doc, yyjson_mut_obj_add_obj(doc, root, "modeSettings"));
        mode.WriteSettings(settings);
    }

    // Notes go one per line (below), so they're written separately from the indented document.
    yyjson_mut_obj_add_arr(doc, root, "notes");

    size_t length = 0;
    char* text = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY_TWO_SPACES, &length);
    if (text == nullptr)
    {
        return Error{"Could not serialize the chart"};
    }
    std::string output(text, length);
    std::free(text);

    std::string notes = "[";
    for (size_t i = 0; i < chart.Notes.size(); ++i)
    {
        const std::unique_ptr<yyjson_mut_doc, DocumentDeleter> noteDocument(yyjson_mut_doc_new(nullptr));
        yyjson_mut_val* object = yyjson_mut_obj(noteDocument.get());
        yyjson_mut_doc_set_root(noteDocument.get(), object);
        yyjson_mut_obj_add_sint(noteDocument.get(), object, "timeUs", chart.Notes[i].Time);
        if (chart.Notes[i].HasDuration())
        {
            yyjson_mut_obj_add_sint(noteDocument.get(), object, "endTimeUs", chart.Notes[i].EndTime);
        }
        if (mode.WriteNote)
        {
            ChartFieldWriter note(noteDocument.get(), object);
            mode.WriteNote(note, i);
        }
        if (chart.Notes[i].Sample != ChartNote::NO_SAMPLE)
        {
            yyjson_mut_obj_add_sint(noteDocument.get(), object, "sample", chart.Notes[i].Sample);
            if (chart.Notes[i].SampleVolume != 100)
            {
                yyjson_mut_obj_add_uint(noteDocument.get(), object, "sampleVolume", chart.Notes[i].SampleVolume);
            }
        }

        char* noteText = yyjson_mut_write(noteDocument.get(), YYJSON_WRITE_NOFLAG, &length);
        if (noteText == nullptr)
        {
            return Error{"Could not serialize note " + std::to_string(i)};
        }
        notes += (i == 0 ? "\n    " : ",\n    ") + std::string(noteText, length);
        std::free(noteText);
    }
    notes += chart.Notes.empty() ? "]" : "\n  ]";

    const std::string placeholder = "\"notes\": []";
    const size_t position = output.rfind(placeholder);
    if (position == std::string::npos)
    {
        return Error{"Could not place the notes in the chart"};
    }
    output.replace(position + placeholder.size() - 2, 2, notes);
    return output + "\n";
}

} // namespace hyoshi::rhythm
