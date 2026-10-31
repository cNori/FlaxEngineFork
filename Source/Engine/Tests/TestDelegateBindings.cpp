// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Core/Delegate.h"
#include "Engine/Core/DelegateBindings.h"
#include <ThirdParty/catch2/catch.hpp>

#if FLAX_DELEGATE_BINDINGS_ENABLED
namespace
{
    struct BindingSink
    {
        int Calls = 0;
        void Invoke(int value) { Calls += value; }
    };
}

TEST_CASE("Delegate external subscription lifetime", "[Core][Delegate]")
{
    BindingSink sink;
    Delegate<int> source;
    source.Bind<BindingSink, &BindingSink::Invoke>(&sink);
    auto lifetime = DelegateBindings::Track(&source);
    REQUIRE(lifetime == DelegateBindings::Track(&source));
    bool closed = false;
    lifetime->NonCopyable.insert(&sink);
    lifetime->Closing.emplace(++lifetime->Next, [lifetime, &sink, &closed](void* address)
    {
        CHECK(lifetime->Source == nullptr);
        static_cast<Delegate<int>*>(address)->Unbind<BindingSink, &BindingSink::Invoke>(&sink);
        lifetime->NonCopyable.erase(&sink);
        closed = true;
    });
    Delegate<int> copy(source);
    copy(10);
    CHECK(sink.Calls == 0);
    Delegate<int>::FunctionType listed[1];
    CHECK(source.GetBindings(listed, 1) == 0);
    source(2);
    CHECK(sink.Calls == 2);
    Delegate<int> moved(MoveTemp(source));
    CHECK(closed);
    CHECK(lifetime->Source == nullptr);
    moved(10);
    CHECK(sink.Calls == 2);
}

TEST_CASE("Delegate destruction retires before callback storage", "[Core][Delegate]")
{
    BindingSink sink;
    auto* source = New<Delegate<int>>();
    source->Bind<BindingSink, &BindingSink::Invoke>(&sink);
    auto lifetime = DelegateBindings::Track(source);
    bool closed = false;
    lifetime->Closing.emplace(++lifetime->Next, [&sink, &closed](void* address)
    {
        auto* live = static_cast<Delegate<int>*>(address);
        CHECK(live->Count() == 1);
        live->Unbind<BindingSink, &BindingSink::Invoke>(&sink);
        closed = true;
    });
    Delete(source);
    CHECK(closed);
    CHECK(lifetime->Source == nullptr);
    CHECK(lifetime->Closing.empty());
}

TEST_CASE("Delegate binding export respects buffer capacity", "[Core][Delegate]")
{
    BindingSink first, second;
    Delegate<int> source;
    source.Bind<BindingSink, &BindingSink::Invoke>(&first);
    source.Bind<BindingSink, &BindingSink::Invoke>(&second);
    struct Buffer
    {
        Delegate<int>::FunctionType Values[1];
        uint64 Canary = 0x123456789abcdef0ULL;
    } buffer;
    CHECK(source.GetBindings(buffer.Values, 1) == 1);
    CHECK(buffer.Canary == 0x123456789abcdef0ULL);
    CHECK(source.GetBindings(nullptr, 1) == 0);
    CHECK(source.GetBindings(buffer.Values, 0) == 0);
}
#endif
