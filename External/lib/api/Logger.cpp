// Copyright (c) Microsoft. All rights reserved.

#include "Logger.hpp"
#include "LogSessionData.hpp"
#include "utils/Utils.hpp"

#include <algorithm>
#include <array>

namespace ARIASDK_NS_BEGIN {

#if 0
    // TODO: [MG] - expose it as internal JSON shadow post
    void SendAsJSON(const EventProperties& props, const std::string& token);
#endif

    Logger::Logger(std::string const& tenantToken, std::string const& source, std::string const& experimentationProject,
        ILogManagerInternal& logManager, ContextFieldsProvider& parentContext, IRuntimeConfig& runtimeConfig,
        IEventFilter& eventFilter)
        :
        m_tenantToken(tenantToken),
        m_source(source),
        m_logManager(logManager),
        m_context(parentContext),
        m_config(runtimeConfig),
        m_eventFilter(eventFilter),

        m_baseDecorator(logManager),
        m_runtimeConfigDecorator(logManager, m_config, tenantTokenToId(tenantToken), experimentationProject),
        m_semanticContextDecorator(logManager),
        m_eventPropertiesDecorator(logManager),
        m_semanticApiDecorators(logManager),

        m_sessionStartTime(0),
        m_sessionId("")
    {
        std::string tenantId = tenantTokenToId(m_tenantToken);
        LOG_TRACE("%p: New instance (tenantId=%s)", this, tenantId.c_str());
        m_iKey = "o:" + tenantId;
    }

    Logger::~Logger()
    {
        LOG_TRACE("%p: Destroyed", this);
    }

    ISemanticContext* Logger::GetSemanticContext() const
    {
        return (ISemanticContext*)(&m_context);
    }

    /******************************************************************************
    * Logger::SetContext
    *
    * Set app/session context fields.
    *
    * Could be used to overwrite the auto-populated(Part A) context fields
    * (ie. m_commonContextFields)
    *
    ******************************************************************************/
    void Logger::SetContext(const std::string& name, EventProperty prop)
    {
        LOG_TRACE("%p: SetContext( properties.name=\"%s\", properties.value=\"%s\", PII=%u, ...)",
            this, name.c_str(), prop.to_string().c_str(), prop.piiKind);

        EventRejectedReason isValidPropertyName = validatePropertyName(name);
        if (isValidPropertyName != REJECTED_REASON_OK)
        {
            LOG_ERROR("Context name is invalid: %s", name.c_str());
            DebugEvent evt;
            evt.type = DebugEventType::EVT_REJECTED;
            evt.param1 = isValidPropertyName;
            DispatchEvent(evt);
            return;
        }

        // Always overwrite the stored value. 
        // Empty string is alowed to remove the previously set value.
        // If the value is empty, the context will not be added to event.
        m_context.setCustomField(name, prop);
    }

    void Logger::SetContext(const std::string& k, const char       v[], PiiKind pii) { SetContext(k, EventProperty(v, pii)); };

    void Logger::SetContext(const std::string& k, const std::string& v, PiiKind pii) { SetContext(k, EventProperty(v, pii)); };

    void Logger::SetContext(const std::string& k, double             v, PiiKind pii) { SetContext(k, EventProperty(v, pii)); };

    void Logger::SetContext(const std::string& k, int64_t            v, PiiKind pii) { SetContext(k, EventProperty(v, pii)); };

    void Logger::SetContext(const std::string& k, time_ticks_t       v, PiiKind pii) { SetContext(k, EventProperty(v, pii)); };

    void Logger::SetContext(const std::string& k, GUID_t             v, PiiKind pii) { SetContext(k, EventProperty(v, pii)); };

    void Logger::SetContext(const std::string& k, bool               v, PiiKind pii) { SetContext(k, EventProperty(v, pii)); };

    /// <summary>
    /// Logs the application lifecycle.
    /// </summary>
    /// <param name="state">The state.</param>
    /// <param name="properties">The properties.</param>
    void Logger::LogAppLifecycle(AppLifecycleState state, EventProperties const& properties)
    {
        LOG_TRACE("%p: LogAppLifecycle(state=%u, properties.name=\"%s\", ...)",
            this, state, properties.GetName().empty() ? "<unnamed>" : properties.GetName().c_str());

        EventLatency latency = EventLatency_Normal;
        ::AriaProtocol::Record record;

        bool decorated =
            applyCommonDecorators(record, properties, latency) &&
            m_semanticApiDecorators.decorateAppLifecycleMessage(record, state);
        if (!decorated)
        {
            LOG_ERROR("Failed to log %s event %s/%s: invalid arguments provided",
                "AppLifecycle", tenantTokenToId(m_tenantToken).c_str(), properties.GetName().empty() ? "<unnamed>" : properties.GetName().c_str());
            return;
        }

        submit(record, latency, properties.GetPersistence(), properties.GetPolicyBitFlags());
        DispatchEvent(DebugEventType::EVT_LOG_LIFECYCLE);
    }

    /// <summary>
    /// Logs the custom event with the specified name.
    /// </summary>
    /// <param name="name">A string that contains the name of the custom event.</param>
    void Logger::LogEvent(std::string const& name)
    {
        EventProperties event(name);
        LogEvent(event);
    }

    /// <summary>
    /// Logs the event.
    /// </summary>
    /// <param name="properties">The properties.</param>
    void Logger::LogEvent(EventProperties const& properties)
    {
        // SendAsJSON(properties, m_tenantToken);

        LOG_TRACE("%p: LogEvent(properties.name=\"%s\", ...)",
            this, properties.GetName().empty() ? "<unnamed>" : properties.GetName().c_str());

        EventLatency latency = EventLatency_Normal;
        if (properties.GetLatency() > EventLatency_Unspecified)
        {
            latency = properties.GetLatency();
        }

        ::AriaProtocol::Record record;

        if (!applyCommonDecorators(record, properties, latency)) {
            LOG_ERROR("Failed to log %s event %s/%s: invalid arguments provided",
                "custom",
                tenantTokenToId(m_tenantToken).c_str(),
                properties.GetName().empty() ? "<unnamed>" : properties.GetName().c_str());
            return;
        }

#if 1 /* Send to shadow */
        //   SendAsJSON(properties, m_tenantToken);
#endif

        submit(record, latency, properties.GetPersistence(), properties.GetPolicyBitFlags());
        DispatchEvent(DebugEventType::EVT_LOG_EVENT);
    }

    /// <summary>
    /// Logs a failure event - such as an application exception.
    /// </summary>
    /// <param name="signature">A string that contains the signature that identifies the bucket of the failure.</param>
    /// <param name="detail">A string that contains a description of the failure.</param>
    /// <param name="category">A string that contains the category of the failure - such as an application error,
    /// application not responding, or application crash</param>
    /// <param name="id">A string that contains the identifier that uniquely identifies this failure.</param>
    /// <param name="properties">Properties of this failure event, specified using an EventProperties object.</param>
    void Logger::LogFailure(
        std::string const& signature,
        std::string const& detail,
        std::string const& category,
        std::string const& id,
        EventProperties const& properties)
    {
        LOG_TRACE("%p: LogFailure(signature=\"%s\", properties.name=\"%s\", ...)",
            this, signature.c_str(), properties.GetName().empty() ? "<unnamed>" : properties.GetName().c_str());

        EventLatency latency = EventLatency_Normal;
        ::AriaProtocol::Record record;

        bool decorated =
            applyCommonDecorators(record, properties, latency) &&
            m_semanticApiDecorators.decorateFailureMessage(record, signature, detail, category, id);

        if (!decorated)
        {
            LOG_ERROR("Failed to log %s event %s/%s: invalid arguments provided",
                "Failure",
                tenantTokenToId(m_tenantToken).c_str(), properties.GetName().empty() ? "<unnamed>" : properties.GetName().c_str());
            return;
        }

        submit(record, latency, properties.GetPersistence(), properties.GetPolicyBitFlags());
        DispatchEvent(DebugEventType::EVT_LOG_FAILURE);
    }

    void Logger::LogFailure(
        std::string const& signature,
        std::string const& detail,
        EventProperties const& properties)
    {
        LogFailure(signature, detail, "", "", properties);
    }

    void Logger::LogPageView(
        std::string const& id,
        std::string const& pageName,
        std::string const& category,
        std::string const& uri,
        std::string const& referrer,
        EventProperties const& properties)
    {
        LOG_TRACE("%p: LogPageView(id=\"%s\", properties.name=\"%s\", ...)",
            this, id.c_str(), properties.GetName().empty() ? "<unnamed>" : properties.GetName().c_str());

        EventLatency latency = EventLatency_Normal;
        ::AriaProtocol::Record record;

        bool decorated =
            applyCommonDecorators(record, properties, latency) &&
            m_semanticApiDecorators.decoratePageViewMessage(record, id, pageName, category, uri, referrer);

        if (!decorated)
        {
            LOG_ERROR("Failed to log %s event %s/%s: invalid arguments provided",
                "PageView", tenantTokenToId(m_tenantToken).c_str(), properties.GetName().empty() ? "<unnamed>" : properties.GetName().c_str());
            return;
        }

        submit(record, latency, properties.GetPersistence(), properties.GetPolicyBitFlags());
        DispatchEvent(DebugEventType::EVT_LOG_PAGEVIEW);
    }


    void Logger::LogPageView(
        std::string const& id,
        std::string const& pageName,
        EventProperties const& properties)
    {
        LogPageView(id, pageName, "", "", "", properties);
    }

    void Logger::LogPageAction(
        std::string const& pageViewId,
        ActionType actionType,
        EventProperties const& properties)
    {
        PageActionData pageActionData(pageViewId, actionType);
        LogPageAction(pageActionData, properties);
    }

    void Logger::LogPageAction(
        PageActionData const& pageActionData,
        EventProperties const& properties)
    {
        LOG_TRACE("%p: LogPageAction(pageActionData.actionType=%u, properties.name=\"%s\", ...)",
            this, pageActionData.actionType, properties.GetName().empty() ? "<unnamed>" : properties.GetName().c_str());

        EventLatency latency = EventLatency_Normal;
        ::AriaProtocol::Record record;

        bool decorated =
            applyCommonDecorators(record, properties, latency) &&
            m_semanticApiDecorators.decoratePageActionMessage(record, pageActionData);
        if (!decorated)
        {
            LOG_ERROR("Failed to log %s event %s/%s: invalid arguments provided",
                "PageAction", tenantTokenToId(m_tenantToken).c_str(), properties.GetName().empty() ? "<unnamed>" : properties.GetName().c_str());
            return;
        }

        submit(record, latency, properties.GetPersistence(), properties.GetPolicyBitFlags());
        DispatchEvent(DebugEventType::EVT_LOG_PAGEACTION);
    }

    /// <summary>
    /// Applies the common decorators.
    /// </summary>
    /// <param name="record">The record.</param>
    /// <param name="properties">The properties.</param>
    /// <param name="latency">The latency.</param>
    /// <returns></returns>
    bool Logger::applyCommonDecorators(::AriaProtocol::Record& record, EventProperties const& properties, ::Microsoft::Applications::Events::EventLatency& latency)
    {
        record.name = properties.GetName();
        record.baseType = EVENTRECORD_TYPE_CUSTOM_EVENT;
        if (record.name.empty())
        {
            record.name = "NotSpecified";
        }
        record.iKey = m_iKey;

        // TODO: [MG] - optimize this code
        bool result = true;
        result &= m_baseDecorator.decorate(record);
        result &= m_semanticContextDecorator.decorate(record);
        result &= m_eventPropertiesDecorator.decorate(record, latency, properties);
        result &= m_runtimeConfigDecorator.decorate(record);
        return result;

    }

    void Logger::submit(::AriaProtocol::Record& record,
        ::Microsoft::Applications::Events::EventLatency latency,
        ::Microsoft::Applications::Events::EventPersistence persistence,
        std::uint64_t  const& policyBitFlags)
    {
        if (latency == EventLatency_Off)
        {
            DispatchEvent(DebugEventType::EVT_DROPPED);
            LOG_INFO("Event %s/%s dropped because of calculated latency 0 (Off)",
                tenantTokenToId(m_tenantToken).c_str(), record.baseType.c_str());
            return;
        }

#ifdef ENABLE_FILTERING /* XXX: [MG] - temporarily disable  */
        if (m_eventFilter.IsEventExcluded(record.name))
        {
            DispatchEvent(DebugEventType::EVT_FILTERED);
            LOG_INFO("Event %s/%s removed due to event filter",
                tenantTokenToId(m_tenantToken).c_str(),
                record.name.c_str());
            return;
        }
#endif

        // TODO: [MG] - check if optimization is possible in generateUuidString
        IncomingEventContext event(PAL::generateUuidString(), m_tenantToken, latency, persistence, &record);
        event.policyBitFlags = policyBitFlags;
        m_logManager.sendEvent(&event);
    }

    void Logger::onSubmitted()
    {
        LOG_INFO("This method is executed from worker thread");
    }

    void Logger::LogSampledMetric(
        std::string const& name,
        double value,
        std::string const& units,
        std::string const& instanceName,
        std::string const& objectClass,
        std::string const& objectId,
        EventProperties const& properties)
    {
        LOG_TRACE("%p: LogSampledMetric(name=\"%s\", properties.name=\"%s\", ...)",
            this, name.c_str(), properties.GetName().empty() ? "<unnamed>" : properties.GetName().c_str());

        EventLatency latency = EventLatency_Normal;
        ::AriaProtocol::Record record;

        bool decorated =
            applyCommonDecorators(record, properties, latency) &&
            m_semanticApiDecorators.decorateSampledMetricMessage(record, name, value, units, instanceName, objectClass, objectId);

        if (!decorated)
        {
            LOG_ERROR("Failed to log %s event %s/%s: invalid arguments provided",
                "SampledMetric", tenantTokenToId(m_tenantToken).c_str(), properties.GetName().empty() ? "<unnamed>" : properties.GetName().c_str());
            return;
        }

        submit(record, latency, properties.GetPersistence(), properties.GetPolicyBitFlags());
        DispatchEvent(DebugEventType::EVT_LOG_SAMPLEMETR);
    }

    void Logger::LogSampledMetric(
        std::string const& name,
        double value,
        std::string const& units,
        EventProperties const& properties)
    {
        LogSampledMetric(name, value, units, "", "", "", properties);
    }

    void Logger::LogAggregatedMetric(
        std::string const& name,
        long duration,
        long count,
        EventProperties const& properties)
    {
        AggregatedMetricData metricData(name, duration, count);
        LogAggregatedMetric(metricData, properties);
    }

    void Logger::LogAggregatedMetric(
        AggregatedMetricData const& metricData,
        EventProperties const& properties)
    {
        LOG_TRACE("%p: LogAggregatedMetric(name=\"%s\", properties.name=\"%s\", ...)",
            this, metricData.name.c_str(), properties.GetName().empty() ? "<unnamed>" : properties.GetName().c_str());

        EventLatency latency = EventLatency_Normal;
        ::AriaProtocol::Record record;

        bool decorated =
            applyCommonDecorators(record, properties, latency) &&
            m_semanticApiDecorators.decorateAggregatedMetricMessage(record, metricData);

        if (!decorated)
        {
            LOG_ERROR("Failed to log %s event %s/%s: invalid arguments provided",
                "AggregatedMetric", tenantTokenToId(m_tenantToken).c_str(), properties.GetName().empty() ? "<unnamed>" : properties.GetName().c_str());
            return;
        }

        submit(record, latency, properties.GetPersistence(), properties.GetPolicyBitFlags());
        DispatchEvent(DebugEventType::EVT_LOG_AGGRMETR);
    }

    void Logger::LogTrace(
        TraceLevel level,
        std::string const& message,
        EventProperties const& properties)
    {
        LOG_TRACE("%p: LogTrace(level=%u, properties.name=\"%s\", ...)",
            this, level, properties.GetName().empty() ? "<unnamed>" : properties.GetName().c_str());

        EventLatency latency = EventLatency_Normal;
        ::AriaProtocol::Record record;

        bool decorated =
            applyCommonDecorators(record, properties, latency) &&
            m_semanticApiDecorators.decorateTraceMessage(record, level, message);

        if (!decorated)
        {
            LOG_ERROR("Failed to log %s event %s/%s: invalid arguments provided",
                "Trace", tenantTokenToId(m_tenantToken).c_str(), properties.GetName().empty() ? "<unnamed>" : properties.GetName().c_str());
            return;
        }

        submit(record, latency, properties.GetPersistence(), properties.GetPolicyBitFlags());
        DispatchEvent(DebugEventType::EVT_LOG_TRACE);
    }

    void Logger::LogUserState(
        UserState state,
        long timeToLiveInMillis,
        EventProperties const& properties)
    {
        LOG_TRACE("%p: LogUserState(state=%u, properties.name=\"%s\", ...)",
            this, state, properties.GetName().empty() ? "<unnamed>" : properties.GetName().c_str());

        EventLatency latency = EventLatency_Normal;
        ::AriaProtocol::Record record;

        bool decorated =
            applyCommonDecorators(record, properties, latency) &&
            m_semanticApiDecorators.decorateUserStateMessage(record, state, timeToLiveInMillis);

        if (!decorated)
        {
            LOG_ERROR("Failed to log %s event %s/%s: invalid arguments provided",
                "UserState", tenantTokenToId(m_tenantToken).c_str(), properties.GetName().empty() ? "<unnamed>" : properties.GetName().c_str());
            return;
        }

        submit(record, latency, properties.GetPersistence(), properties.GetPolicyBitFlags());
        DispatchEvent(DebugEventType::EVT_LOG_USERSTATE);
    }

    /******************************************************************************
    * Logger::LogSession
    *
    * Log a user's Session.
    *
    ******************************************************************************/
    void Logger::LogSession(SessionState state, const EventProperties& prop)
    {
        LogSessionData* logSessionData = m_logManager.GetLogSessionData();
        std::string sessionSDKUid = logSessionData->getSessionSDKUid();
        unsigned long long sessionFirstTime = logSessionData->getSesionFirstTime();

        if (sessionSDKUid == "" || sessionFirstTime == 0)
        {
            LOG_WARN("We don't have a first time so no session logged");
            return;
        }

        EventRejectedReason isValidEventName = validateEventName(prop.GetName());
        if (isValidEventName != REJECTED_REASON_OK)
        {
            LOG_ERROR("Invalid event properties!");
            DebugEvent evt;
            evt.type = DebugEventType::EVT_REJECTED;
            evt.param1 = isValidEventName;
            DispatchEvent(evt);
            return;
        }

        int64_t sessionDuration = 0;
        switch (state)
        {
        case SessionState::Session_Started:
        {
            if (m_sessionStartTime > 0)
            {
                LOG_ERROR("LogSession The order is not the correct one in calling LogSession");
                return;
            }
            m_sessionStartTime = PAL::getUtcSystemTime();

            m_sessionId = PAL::generateUuidString();
            break;
        }
        case SessionState::Session_Ended:
        {
            if (m_sessionStartTime == 0)
            {
                LOG_WARN("LogSession We don't have session start time");
                return;
            }
            sessionDuration = PAL::getUtcSystemTime() - m_sessionStartTime;
            break;
        }
        }

        EventLatency latency = EventLatency_RealTime;
        ::AriaProtocol::Record record;

        bool decorated = applyCommonDecorators(record, prop, latency) &&
            m_semanticApiDecorators.decorateSessionMessage(record, state, m_sessionId, PAL::formatUtcTimestampMsAsISO8601(sessionFirstTime), sessionSDKUid, sessionDuration);

        if (!decorated)
        {
            LOG_ERROR("Failed to log %s event %s/%s: invalid arguments provided",
                "Trace", tenantTokenToId(m_tenantToken).c_str(), prop.GetName().empty() ? "<unnamed>" : prop.GetName().c_str());
            return;
        }

        submit(record, latency, prop.GetPersistence(), prop.GetPolicyBitFlags());
        DispatchEvent(DebugEventType::EVT_LOG_SESSION);
    }

    ILogManager& Logger::GetParent()
    {
        return m_logManager;
    }

    LogSessionData* Logger::GetLogSessionData()
    {
        return m_logManager.GetLogSessionData();
    }

    IAuthTokensController*  Logger::GetAuthTokensController()
    {
        return m_logManager.GetAuthTokensController();
    }

    bool Logger::DispatchEvent(DebugEvent evt)
    {
        return m_logManager.DispatchEvent(std::move(evt));
    };

    std::string Logger::GetSource()
    {
        return m_source;
    }

} ARIASDK_NS_END