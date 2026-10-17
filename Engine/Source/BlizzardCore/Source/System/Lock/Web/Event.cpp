#if defined(WOWDECOMP_PLATFORM_WEB)

#include <cstdint>
#include <windows.h>
#include "Engine/Source/BlizzardCore/Source/System/Lock/Lock.h"
#include "Engine/Source/BlizzardCore/Source/System/Lock/Web/WebSync.h"

// Blizzard::Lock::Event is a HANDLE; here it points at an auto-reset WebSync event.
static WebSync::Obj* NewEvent() {
    return WebSync::Create(WebSync::KindEvent, false, false);
}

void Blizzard::Lock::EventCreate(Event& event) {
    event = NewEvent();
}

void Blizzard::Lock::EventDestroy(Event& event) {
    WebSync::Release(static_cast<WebSync::Obj*>(event));
}

void Blizzard::Lock::EventSet(Event& event) {
    WebSync::SetSignaled(static_cast<WebSync::Obj*>(event), true);
}

void Blizzard::Lock::EventWaitForever(Event& event) {
    WebSync::Wait(static_cast<WebSync::Obj*>(event), WebSync::kInfinite);
}

Blizzard::Lock::Event* Blizzard::Lock::EventWait(uint32_t timeout, Event& event) {
    return WebSync::Wait(static_cast<WebSync::Obj*>(event), timeout) == 0 ? &event : nullptr;
}

Blizzard::Lock::EventObject::EventObject() {
    this->m_event = NewEvent();
}

Blizzard::Lock::EventObject::~EventObject() {
    EventDestroy(this->m_event);
}

#endif
