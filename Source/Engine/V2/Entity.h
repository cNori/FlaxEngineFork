#pragma once

#include <Engine/Core/Types/BaseTypes.h>

struct Entity
{
    class SharedID
    {
        struct State
        {
            uint32 ID = 0;
            uint32 Refs = 1;
            void* Owner = nullptr;
            void (*OnLastRelease)(void*, uint32) = nullptr;
        };

        State* _state = nullptr;

        static void Retain(State* state)
        {
            if (state)
            {
                ASSERT(state->Refs < MAX_uint32);
                ++state->Refs;
            }
        }

        void Reset()
        {
            State* state = _state;
            _state = nullptr;

            if (state && --state->Refs == 0)
            {
                if (state->OnLastRelease)
                    state->OnLastRelease(state->Owner, state->ID);
                delete state;
            }
        }

    public:
        SharedID() = default;

        SharedID(uint32 id, void* owner = nullptr,
            void (*onLastRelease)(void*, uint32) = nullptr)
            : _state(new State{ id, 1, owner, onLastRelease })
        {
            ASSERT(id != 0);
        }

        SharedID(const SharedID& other)
            : _state(other._state)
        {
            Retain(_state);
        }

        SharedID& operator=(const SharedID& other)
        {
            if (this != &other)
            {
                Retain(other._state); // retain first in case both share State
                Reset();
                _state = other._state;
            }
            return *this;
        }

        SharedID(SharedID&& other) noexcept
            : _state(other._state)
        {
            other._state = nullptr;
        }

        SharedID& operator=(SharedID&& other) noexcept
        {
            if (this != &other)
            {
                Reset();
                _state = other._state;
                other._state = nullptr;
            }
            return *this;
        }

        ~SharedID()
        {
            Reset();
        }

        uint32 GetID() const
        {
            return _state ? _state->ID : MAX_uint32;
        }

        explicit operator bool() const
        {
            return GetID() != MAX_uint32;
        }
    };
    SharedID id;
    static const uint32 invalidindex = MAX_uint32;
private:
    Entity() { this->id = invalidindex; }
    Entity(uint32 id) { this->id = id; }
    bool IsInvalid()
    {
        return id.GetID() == invalidindex;
    }
};
