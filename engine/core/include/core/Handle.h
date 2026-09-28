#pragma once

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace hyoshi
{

// Typed generational handle. The tag makes handles to different resource kinds distinct types.
// Generation 0 is never issued, so a default-constructed handle is invalid.
template <typename Tag>
struct Handle
{
    uint32_t Index = 0;
    uint32_t Generation = 0;

    bool IsValid() const
    {
        return Generation != 0;
    }

    friend bool operator==(const Handle&, const Handle&) = default;
};

// Owns objects of type T and hands out generational handles to them. A handle to a destroyed
// object is detected as stale rather than silently aliasing whatever reuses its slot.
template <typename T, typename Tag = T>
class HandlePool
{
public:
    using HandleType = Handle<Tag>;

    HandleType Create(T value)
    {
        uint32_t index = 0;
        if (!freeList.empty())
        {
            index = freeList.back();
            freeList.pop_back();
        }
        else
        {
            index = static_cast<uint32_t>(slots.size());
            slots.emplace_back();
        }

        Slot& slot = slots[index];
        slot.Value.emplace(std::move(value));
        ++liveCount;
        return HandleType{index, slot.Generation};
    }

    // Returns false if the handle is invalid or already destroyed.
    bool Destroy(HandleType handle)
    {
        Slot* slot = FindSlot(handle);
        if (slot == nullptr)
        {
            return false;
        }

        slot->Value.reset();
        slot->Generation = NextGeneration(slot->Generation);
        freeList.push_back(handle.Index);
        --liveCount;
        return true;
    }

    // Returns nullptr if the handle is invalid or stale. The pointer is invalidated by Create.
    T* Get(HandleType handle)
    {
        Slot* slot = FindSlot(handle);
        return slot != nullptr ? &*slot->Value : nullptr;
    }

    const T* Get(HandleType handle) const
    {
        const Slot* slot = FindSlot(handle);
        return slot != nullptr ? &*slot->Value : nullptr;
    }

    bool Contains(HandleType handle) const
    {
        return FindSlot(handle) != nullptr;
    }

    uint32_t Size() const
    {
        return liveCount;
    }

    bool IsEmpty() const
    {
        return liveCount == 0;
    }

    // Calls fn(T&) for every live object, in slot order.
    template <typename Fn>
    void ForEach(Fn&& fn)
    {
        for (Slot& slot : slots)
        {
            if (slot.Value.has_value())
            {
                fn(*slot.Value);
            }
        }
    }

    // Destroys every object. Existing handles become stale.
    void Clear()
    {
        for (uint32_t index = 0; index < slots.size(); ++index)
        {
            Slot& slot = slots[index];
            if (slot.Value.has_value())
            {
                slot.Value.reset();
                slot.Generation = NextGeneration(slot.Generation);
                freeList.push_back(index);
            }
        }
        liveCount = 0;
    }

private:
    struct Slot
    {
        std::optional<T> Value;
        uint32_t Generation = 1;
    };

    static uint32_t NextGeneration(uint32_t generation)
    {
        ++generation;
        return generation == 0 ? 1 : generation;
    }

    Slot* FindSlot(HandleType handle)
    {
        return const_cast<Slot*>(std::as_const(*this).FindSlot(handle));
    }

    const Slot* FindSlot(HandleType handle) const
    {
        if (handle.Index >= slots.size())
        {
            return nullptr;
        }

        const Slot& slot = slots[handle.Index];
        if (slot.Generation != handle.Generation || !slot.Value.has_value())
        {
            return nullptr;
        }

        return &slot;
    }

    std::vector<Slot> slots;
    std::vector<uint32_t> freeList;
    uint32_t liveCount = 0;
};

} // namespace hyoshi
