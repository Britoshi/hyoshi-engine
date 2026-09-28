#include "renderer/Image.h"

#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>

namespace hyoshi::renderer
{

namespace
{

// Alpha at or above this is inside the artwork. Trimming keeps a few pixels beyond, for the
// anti-aliased edge.
constexpr uint8_t INSIDE_ALPHA = 128;
constexpr uint32_t TRIM_MARGIN = 2;
// The field is computed at up to this many times the output resolution, then resampled.
constexpr uint32_t OVERSAMPLE = 3;
// "No seed here" in the distance transform: far beyond any real squared distance.
constexpr float FAR = 1.0e20f;

uint8_t GetAlpha(const Image& image, size_t pixel)
{
    return image.Channels == 4 ? image.Pixels[(pixel * 4) + 3] : image.Pixels[pixel];
}

// Clears the connected shapes (8-connected pixels at INSIDE_ALPHA or more, with the fainter
// pixels around them) that are smaller than `fraction` of the largest.
void RemoveSpecks(std::vector<uint8_t>& alpha, uint32_t width, uint32_t height, float fraction)
{
    std::vector<int32_t> labels(alpha.size(), -1);
    std::vector<size_t> sizes;
    std::vector<size_t> stack;
    for (size_t start = 0; start < alpha.size(); ++start)
    {
        if (alpha[start] < INSIDE_ALPHA || labels[start] >= 0)
        {
            continue;
        }
        const auto label = static_cast<int32_t>(sizes.size());
        size_t count = 0;
        labels[start] = label;
        stack.push_back(start);
        while (!stack.empty())
        {
            const size_t pixel = stack.back();
            stack.pop_back();
            ++count;
            const auto x = static_cast<int64_t>(pixel % width);
            const auto y = static_cast<int64_t>(pixel / width);
            for (int64_t dy = -1; dy <= 1; ++dy)
            {
                for (int64_t dx = -1; dx <= 1; ++dx)
                {
                    const int64_t nx = x + dx;
                    const int64_t ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= width || ny >= height)
                    {
                        continue;
                    }
                    const size_t neighbor = (static_cast<size_t>(ny) * width) + static_cast<size_t>(nx);
                    if (alpha[neighbor] >= INSIDE_ALPHA && labels[neighbor] < 0)
                    {
                        labels[neighbor] = label;
                        stack.push_back(neighbor);
                    }
                }
            }
        }
        sizes.push_back(count);
    }
    if (sizes.empty())
    {
        return;
    }
    const auto threshold =
        static_cast<size_t>(static_cast<float>(*std::max_element(sizes.begin(), sizes.end())) * fraction);
    // A speck's faint fringe goes with it: any pixel within two of a removed one.
    std::vector<bool> isRemoved(alpha.size(), false);
    for (size_t pixel = 0; pixel < alpha.size(); ++pixel)
    {
        if (labels[pixel] >= 0 && sizes[static_cast<size_t>(labels[pixel])] < threshold)
        {
            isRemoved[pixel] = true;
        }
    }
    for (size_t pixel = 0; pixel < alpha.size(); ++pixel)
    {
        if (!isRemoved[pixel])
        {
            continue;
        }
        const auto x = static_cast<int64_t>(pixel % width);
        const auto y = static_cast<int64_t>(pixel / width);
        for (int64_t ny = std::max<int64_t>(y - 2, 0); ny <= std::min<int64_t>(y + 2, height - 1); ++ny)
        {
            for (int64_t nx = std::max<int64_t>(x - 2, 0); nx <= std::min<int64_t>(x + 2, width - 1); ++nx)
            {
                const size_t neighbor = (static_cast<size_t>(ny) * width) + static_cast<size_t>(nx);
                if (labels[neighbor] < 0 || isRemoved[neighbor])
                {
                    alpha[neighbor] = 0;
                }
            }
        }
    }
}

// Squared distances to the nearest zero of f along one line (Felzenszwalb and Huttenlocher's
// lower envelope of parabolas). f holds 0 at seeds and FAR elsewhere; v and z are scratch.
void TransformLine(const float* f, float* out, int count, int* v, float* z)
{
    int k = 0;
    v[0] = 0;
    z[0] = -FAR;
    z[1] = FAR;
    for (int q = 1; q < count; ++q)
    {
        const auto fq = f[q] + static_cast<float>(q * q);
        float s = (fq - (f[v[k]] + static_cast<float>(v[k] * v[k]))) / static_cast<float>(2 * (q - v[k]));
        while (s <= z[k])
        {
            --k;
            s = (fq - (f[v[k]] + static_cast<float>(v[k] * v[k]))) / static_cast<float>(2 * (q - v[k]));
        }
        ++k;
        v[k] = q;
        z[k] = s;
        z[k + 1] = FAR;
    }
    k = 0;
    for (int q = 0; q < count; ++q)
    {
        while (z[k + 1] < static_cast<float>(q))
        {
            ++k;
        }
        const auto offset = static_cast<float>(q - v[k]);
        out[q] = (offset * offset) + f[v[k]];
    }
}

// Squared Euclidean distance from every cell to the nearest seed, in place: columns, then rows.
void TransformGrid(std::vector<float>& grid, int width, int height)
{
    const int longest = std::max(width, height);
    std::vector<float> line(static_cast<size_t>(longest));
    std::vector<float> result(static_cast<size_t>(longest));
    std::vector<int> v(static_cast<size_t>(longest));
    std::vector<float> z(static_cast<size_t>(longest) + 1);
    for (int x = 0; x < width; ++x)
    {
        for (int y = 0; y < height; ++y)
        {
            line[static_cast<size_t>(y)] =
                grid[(static_cast<size_t>(y) * static_cast<size_t>(width)) + static_cast<size_t>(x)];
        }
        TransformLine(line.data(), result.data(), height, v.data(), z.data());
        for (int y = 0; y < height; ++y)
        {
            grid[(static_cast<size_t>(y) * static_cast<size_t>(width)) + static_cast<size_t>(x)] =
                result[static_cast<size_t>(y)];
        }
    }
    for (int y = 0; y < height; ++y)
    {
        float* row = &grid[static_cast<size_t>(y) * static_cast<size_t>(width)];
        TransformLine(row, result.data(), width, v.data(), z.data());
        std::memcpy(row, result.data(), static_cast<size_t>(width) * sizeof(float));
    }
}

} // namespace

Result<Image> DecodeImage(std::span<const std::byte> data)
{
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(data.data()),
                                            static_cast<int>(data.size()), &width, &height, &channels, 4);
    if (pixels == nullptr)
    {
        return Error{std::string("Could not decode the image: ") + stbi_failure_reason()};
    }
    Image image;
    image.Width = static_cast<uint32_t>(width);
    image.Height = static_cast<uint32_t>(height);
    image.Channels = 4;
    image.Pixels.assign(pixels, pixels + (static_cast<size_t>(width) * static_cast<size_t>(height) * 4));
    stbi_image_free(pixels);
    return image;
}

Image MakeDistanceField(const Image& image, uint32_t maxSize, uint32_t spread, float speckFraction)
{
    std::vector<uint8_t> alpha(size_t{image.Width} * image.Height);
    for (size_t pixel = 0; pixel < alpha.size(); ++pixel)
    {
        alpha[pixel] = GetAlpha(image, pixel);
    }
    if (speckFraction > 0.0f)
    {
        RemoveSpecks(alpha, image.Width, image.Height, speckFraction);
    }

    // The artwork's bounds, without transparent margins.
    uint32_t minX = image.Width;
    uint32_t minY = image.Height;
    uint32_t maxX = 0;
    uint32_t maxY = 0;
    for (uint32_t y = 0; y < image.Height; ++y)
    {
        for (uint32_t x = 0; x < image.Width; ++x)
        {
            if (alpha[(size_t{y} * image.Width) + x] >= INSIDE_ALPHA)
            {
                minX = std::min(minX, x);
                minY = std::min(minY, y);
                maxX = std::max(maxX, x);
                maxY = std::max(maxY, y);
            }
        }
    }
    if (minX > maxX || maxSize <= (2 * spread) + 1)
    {
        return {};
    }
    minX = minX > TRIM_MARGIN ? minX - TRIM_MARGIN : 0;
    minY = minY > TRIM_MARGIN ? minY - TRIM_MARGIN : 0;
    maxX = std::min(maxX + TRIM_MARGIN, image.Width - 1);
    maxY = std::min(maxY + TRIM_MARGIN, image.Height - 1);
    const uint32_t contentWidth = maxX - minX + 1;
    const uint32_t contentHeight = maxY - minY + 1;
    const uint32_t contentLongest = std::max(contentWidth, contentHeight);
    const uint32_t outputContent = maxSize - (2 * spread);

    // Box-downsample the coverage to about OVERSAMPLE times the output, never above the source.
    const uint32_t factor = std::max(1u, contentLongest / (outputContent * OVERSAMPLE));
    const uint32_t workContentWidth = (contentWidth + factor - 1) / factor;
    const uint32_t workContentHeight = (contentHeight + factor - 1) / factor;
    const float workPerOutput =
        static_cast<float>(std::max(workContentWidth, workContentHeight)) / static_cast<float>(outputContent);
    const auto padding = static_cast<uint32_t>(std::ceil(static_cast<float>(spread) * workPerOutput)) + 1;
    const int workWidth = static_cast<int>(workContentWidth + (2 * padding));
    const int workHeight = static_cast<int>(workContentHeight + (2 * padding));

    std::vector<float> coverage(static_cast<size_t>(workWidth) * static_cast<size_t>(workHeight), 0.0f);
    for (uint32_t wy = 0; wy < workContentHeight; ++wy)
    {
        for (uint32_t wx = 0; wx < workContentWidth; ++wx)
        {
            uint32_t sum = 0;
            uint32_t count = 0;
            for (uint32_t sy = wy * factor; sy < std::min((wy + 1) * factor, contentHeight); ++sy)
            {
                for (uint32_t sx = wx * factor; sx < std::min((wx + 1) * factor, contentWidth); ++sx)
                {
                    sum += alpha[(size_t{minY + sy} * image.Width) + minX + sx];
                    ++count;
                }
            }
            coverage[(size_t{wy + padding} * static_cast<size_t>(workWidth)) + wx + padding] =
                static_cast<float>(sum) / (255.0f * static_cast<float>(count));
        }
    }

    // Distances to the nearest inside and outside cells, signed: positive inside. Partly covered
    // cells on the edge use their coverage for sub-cell precision; faint cells away from any
    // edge (nearly transparent noise) keep their true distance.
    std::vector<float> toInside(coverage.size());
    std::vector<float> toOutside(coverage.size());
    for (size_t i = 0; i < coverage.size(); ++i)
    {
        const bool isInside = coverage[i] >= 0.5f;
        toInside[i] = isInside ? 0.0f : FAR;
        toOutside[i] = isInside ? FAR : 0.0f;
    }
    TransformGrid(toInside, workWidth, workHeight);
    TransformGrid(toOutside, workWidth, workHeight);
    std::vector<float> signedDistance(coverage.size());
    for (size_t i = 0; i < coverage.size(); ++i)
    {
        const bool isInside = coverage[i] >= 0.5f;
        const float toOtherSide = std::sqrt(isInside ? toOutside[i] : toInside[i]);
        signedDistance[i] = isInside ? toOtherSide - 0.5f : 0.5f - toOtherSide;
        if (coverage[i] > 0.0f && coverage[i] < 1.0f && toOtherSide <= 1.5f)
        {
            signedDistance[i] = coverage[i] - 0.5f;
        }
    }

    // Resample to the output resolution, with `spread` texels of field around the shape.
    Image field;
    field.Channels = 1;
    field.Width =
        static_cast<uint32_t>(std::lround(static_cast<float>(workContentWidth) / workPerOutput)) + (2 * spread);
    field.Height =
        static_cast<uint32_t>(std::lround(static_cast<float>(workContentHeight) / workPerOutput)) + (2 * spread);
    field.Pixels.resize(size_t{field.Width} * field.Height);
    auto sample = [&](float x, float y)
    {
        x = std::clamp(x, 0.0f, static_cast<float>(workWidth - 1));
        y = std::clamp(y, 0.0f, static_cast<float>(workHeight - 1));
        const int x0 = std::min(static_cast<int>(x), workWidth - 2);
        const int y0 = std::min(static_cast<int>(y), workHeight - 2);
        const float tx = x - static_cast<float>(x0);
        const float ty = y - static_cast<float>(y0);
        auto at = [&](int cx, int cy)
        {
            return signedDistance[(static_cast<size_t>(cy) * static_cast<size_t>(workWidth)) + static_cast<size_t>(cx)];
        };
        const float top = at(x0, y0) + ((at(x0 + 1, y0) - at(x0, y0)) * tx);
        const float bottom = at(x0, y0 + 1) + ((at(x0 + 1, y0 + 1) - at(x0, y0 + 1)) * tx);
        return top + ((bottom - top) * ty);
    };
    // Output texel (spread, spread) starts where the work grid's content starts.
    const float originX = static_cast<float>(padding) - (static_cast<float>(spread) * workPerOutput);
    const float originY = originX;
    for (uint32_t y = 0; y < field.Height; ++y)
    {
        for (uint32_t x = 0; x < field.Width; ++x)
        {
            const float workX = originX + ((static_cast<float>(x) + 0.5f) * workPerOutput) - 0.5f;
            const float workY = originY + ((static_cast<float>(y) + 0.5f) * workPerOutput) - 0.5f;
            const float distance = sample(workX, workY) / workPerOutput;
            const float value = 0.5f + (distance / (2.0f * static_cast<float>(spread)));
            field.Pixels[(size_t{y} * field.Width) + x] =
                static_cast<uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
        }
    }
    return field;
}

Result<rhi::TextureHandle> CreateTexture(rhi::IRenderDevice& device, const Image& image, const char* debugName)
{
    if (image.IsEmpty() || (image.Channels != 4 && image.Channels != 1))
    {
        return Error{std::string("CreateTexture: an empty or unsupported image (") + debugName + ")"};
    }
    rhi::TextureDesc desc;
    desc.Width = image.Width;
    desc.Height = image.Height;
    desc.Format = image.Channels == 4 ? rhi::TextureFormat::Rgba8Unorm : rhi::TextureFormat::R8Unorm;
    desc.DebugName = debugName;
    Result<rhi::TextureHandle> texture = device.CreateTexture(desc);
    if (!texture)
    {
        return texture;
    }
    if (Result<void> result =
            device.UpdateTexture(texture.Value(), image.Pixels.data(), 0, 0, image.Width, image.Height);
        !result)
    {
        device.Destroy(texture.Value());
        return result.GetError();
    }
    return texture;
}

} // namespace hyoshi::renderer
