#include "DebugEvents.hpp"
#include "utils/Utils.hpp"
#include "pal/PAL.hpp"

#include <atomic>

namespace ARIASDK_NS_BEGIN {

    /// <summary>Add event listener for specific debug event type.</summary>
    void DebugEventSource::AddEventListener(DebugEventType type, DebugEventListener &listener)
    {
        auto &v = listeners[type];
        v.push_back(&listener);
    }

    /// <summary>Remove previously added debug event listener for specific type.</summary>
    void DebugEventSource::RemoveEventListener(DebugEventType type, DebugEventListener &listener)
    {
        auto &v = listeners[type];
        auto it = std::remove(v.begin(), v.end(), &listener);
        v.erase(it, v.end());
    }

    /// <summary>ARIA SDK invokes this method to dispatch event to client callback</summary>
    bool DebugEventSource::DispatchEvent(DebugEvent evt)
    {
        seq++;
        evt.seq = seq;
        evt.ts = PAL::getUtcSystemTime();
        bool dispatched = false;

        if (listeners.size()) {
            // Events filter handlers list
            auto &v = listeners[evt.type];
            for (auto listener : v) {
                listener->OnDebugEvent(evt);
                dispatched = true;
            }
        }

        if (cascaded.size())
        {
            // Cascade event to all other attached sources
            for (auto item : cascaded)
            {
                if (item)
                    item->DispatchEvent(evt);
            }
        }

        return dispatched;
    }

    /// <summary>Attach cascaded DebugEventSource to forward all events to</summary>
    bool DebugEventSource::AttachEventSource(DebugEventSource & other)
    {
        cascaded.insert(&other);
        return true;
    }

    /// <summary>Detach cascaded DebugEventSource to forward all events to</summary>
    bool DebugEventSource::DetachEventSource(DebugEventSource & other)
    {
        return (cascaded.erase(&other)!=0);
    }


} ARIASDK_NS_END
