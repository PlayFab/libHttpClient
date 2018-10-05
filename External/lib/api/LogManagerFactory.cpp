#include "LogManagerFactory.hpp"
#include "LogManagerImpl.hpp"

#include <cstdio>
#include <cstdlib>

#include <atomic>
#include <algorithm>
#include <utility>
#include <functional>
#include <iostream>
#include <cassert>

#include <ctime>

namespace ARIASDK_NS_BEGIN {

    // This mutex has to be recursive because we allow both
    // Destroy and destrutor to lock it. destructor could be
    // called directly, and Destroy calls destructor.
    std::recursive_mutex     ILogManagerInternal::managers_lock;
    std::set<ILogManager*>   ILogManagerInternal::managers;

    /// <summary>
    /// Creates an instance of ILogManager using specified configuration.
    /// </summary>
    /// <param name="configuration">The configuration.</param>
    /// <returns>ILogManager instance</returns>
    ILogManager* LogManagerFactory::Create(ILogConfiguration& configuration)
    {
        LOCKGUARD(ILogManagerInternal::managers_lock);
        auto logManager = new LogManagerImpl(configuration);
        ILogManagerInternal::managers.emplace(logManager);
        return logManager;
    }

    /// <summary>
    /// Destroys the specified ILogManager instance if it's valid.
    /// </summary>
    /// <param name="instance">The instance.</param>
    /// <returns></returns>
    status_t LogManagerFactory::Destroy(ILogManager *instance)
    {
        LOCKGUARD(ILogManagerInternal::managers_lock);
        auto it = ILogManagerInternal::managers.find(instance);
        if (it != std::end(ILogManagerInternal::managers))
        {
            ILogManagerInternal::managers.erase(it);
            delete instance;
            return STATUS_SUCCESS;
        }
        return STATUS_EFAIL;
    }

    // Move guests from ROOT to the new host home
    void LogManagerFactory::rehome(const std::string& name, const std::string& host)
    {
        // copy all items from the ROOT set to their new host
        shared[ANYHOST].first.insert(name);
        // shared[ANYHOST].second->SetKey(host);
        shared[host] = std::move(shared[ANYHOST]);
        shared.erase(ANYHOST);
    }

    const ILogManager* LogManagerFactory::find(const std::string& name)
    {
        for (auto &pool : { shared, exclusive })
            for (auto &kv : pool)
            {
                if (kv.second.first.count(name)) // check set for name
                    return kv.second.second;     // found ILogManager
            }
        return nullptr;
    }

    void LogManagerFactory::parseConfig(ILogConfiguration& c, std::string& name, std::string& host)
    {
        if (c.find("name") != std::end(c))
        {
            name = std::string((const char*)c["name"]);
        }

        auto it = c.find("config");
        if (it != std::end(c))
        {
            if (it->second.type == Variant::TYPE_OBJ)
            {
                const char* config_host = c["config"]["host"];
                host = std::string(config_host);
            }
        }
    }

    ILogManager* LogManagerFactory::lease(ILogConfiguration& c)
    {
        std::string name;
        std::string host;
        parseConfig(c, name, host);

        // Exclusive mode
        if (host.empty())
        {
            // Exclusive hosts are being kept in their own sandbox: high chairs near the bar.
            if (!exclusive.count(name))
                exclusive[name] = { { name }, Create(c) };
            c["hostMode"] = true;
            return exclusive[name].second;
        }

        // Shared mode
        if (shared.size() && (host == ANYHOST))
        {
            // There are some items already. This guest doesn't care
            // where to go, so it goes to the first host's pool.
            shared[shared.begin()->first].first.insert(name);
            c["hostMode"] = false;
            return shared[shared.begin()->first].second;
        }

        if (!shared.count(host))
        {
            // That's a first visitor to this ILogManager$host
            // It is going to be assoc'd with this host LogManager
            // irrespective of whether it's a host or guest
            if (shared.count(ANYHOST))
            {
                rehome(name, host);
            }
            else
            {
                shared[host] = { { name }, Create(c) };
            }
        }
        else
            if (!shared[host].first.count(name))
            {
                // Host already exists, so simply add the new item to it
                shared[host].first.insert(name);
            }

        // TODO: [MG] - if there was no module configuration supplied
        // explicitly, then do we treat the client as host or guest?
        c["hostMode"] = (name==host);
        return shared[host].second;
    }

    bool LogManagerFactory::release(ILogConfiguration& c)
    {
        std::string name, host;
        parseConfig(c, name, host);
        if (host.empty())
        {
            return release(name);
        }
        return release(name, host);
    }

    bool LogManagerFactory::release(const std::string &name)
    {
        for (auto &kv : shared)
        {
            const std::string &host = kv.first;
            if (kv.second.first.count(name))
            {
                kv.second.first.erase(name);
                if (kv.second.first.empty())
                {
                    // Last owner is gone, destroy 
                    if (shared[host].second)
                        delete shared[host].second;
                    shared.erase(host);
                }
                return true;
            }
        }
        return false;
    }

    bool LogManagerFactory::release(const std::string& name, const std::string& host)
    {
        if (host.empty())
        {
            if (exclusive.count(name))
            {
                auto val = exclusive[name];
                // destroy LM
                if (val.second)
                    delete val.second;
                exclusive.erase(name);
                return true;
            }
            return false;
        }

        if ((shared.find(host) != std::end(shared)) &&
            (shared[host].first.count(name)))
        {
            shared[host].first.erase(name);
            if (shared[host].first.empty())
            {
                // Last owner is gone, destroy LM
                if (shared[host].second)
                    delete shared[host].second;
                shared.erase(host);
            }
            return true;
        }

        // repeat the same action as above, but this time
        // searching for rehomed guests in all hosts
        return release(name);
    }

    void LogManagerFactory::dump()
    {
        for (const auto &items : { shared, exclusive })
        {
            for (auto &kv : items)
            {
                std::string csv;
                bool first = true;
                for (auto &name : kv.second.first)
                {
                    if (!first)
                    {
                        csv += ",";
                    }
                    csv += name;
                    first = false;
                }
                LOG_TRACE("[%s]=[%s]", kv.first.c_str(), csv.c_str());
            }
        }
    }

    LogManagerFactory::LogManagerFactory()
    {
    }


    LogManagerFactory::~LogManagerFactory()
    {
    }

} ARIASDK_NS_END