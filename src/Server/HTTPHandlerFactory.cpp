#include <Server/HTTPHandlerFactory.h>

#include <Server/HTTP/HTTPRequestHandler.h>
#include <Server/IServer.h>
#include <Access/Credentials.h>
#include <Interpreters/Session.h>

#include <Poco/Util/LayeredConfiguration.h>

#include "HTTPHandler.h"
#include "NotFoundHandler.h"
#include "StaticRequestHandler.h"
#include "InterserverIOHTTPHandler.h"
#include "PrometheusRequestHandler.h"
#include "RestHTTPRequestHandler.h"
#include "WebUIRequestHandler.h"


namespace DB
{

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
    extern const int UNKNOWN_ELEMENT_IN_CONFIG;
    extern const int INVALID_CONFIG_PARAMETER;
}

static void addCommonDefaultHandlersFactory(HTTPRequestHandlerFactoryMain & factory, IServer & server);
/// proton: starts
static void addDefaultHandlersFactory(HTTPRequestHandlerFactoryMain & factory, IServer & server, AsynchronousMetrics & async_metrics, bool snapshot_mode_ = false);
/// proton: ends

HTTPRequestHandlerFactoryMain::HTTPRequestHandlerFactoryMain(const std::string & name_)
    : log(&Poco::Logger::get(name_)), name(name_)
{
}

std::unique_ptr<HTTPRequestHandler> HTTPRequestHandlerFactoryMain::createRequestHandler(const HTTPServerRequest & request)
{
    LOG_TRACE(log, "HTTP Request for {}. Method: {}, Address: {}, User-Agent: {}{}, Content Type: {}, Transfer Encoding: {}, X-Forwarded-For: {}",
        name, request.getMethod(), request.clientAddress().toString(), request.get("User-Agent", "(none)"),
        (request.hasContentLength() ? (", Length: " + std::to_string(request.getContentLength())) : ("")),
        request.getContentType(), request.getTransferEncoding(), request.get("X-Forwarded-For", "(none)"));

    for (auto & handler_factory : child_factories)
    {
        auto handler = handler_factory->createRequestHandler(request);
        if (handler)
            return handler;
    }

    if (request.getMethod() == Poco::Net::HTTPRequest::HTTP_GET
        || request.getMethod() == Poco::Net::HTTPRequest::HTTP_HEAD
        || request.getMethod() == Poco::Net::HTTPRequest::HTTP_POST)
    {
        return std::unique_ptr<HTTPRequestHandler>(new NotFoundHandler);
    }

    return nullptr;
}

/// proton: starts
static inline auto createHandlersFactoryFromConfig(
    IServer & server, const std::string & name, const String & prefix, AsynchronousMetrics & async_metrics, bool snapshot_mode_ = false)
/// proton: ends
{
    auto main_handler_factory = std::make_shared<HTTPRequestHandlerFactoryMain>(name);

    Poco::Util::AbstractConfiguration::Keys keys;
    server.config().keys(prefix, keys);

    for (const auto & key : keys)
    {
        if (key == "defaults")
        {
            /// proton: starts
            addDefaultHandlersFactory(*main_handler_factory, server, async_metrics, snapshot_mode_);
            /// proton: ends
        }
        else if (startsWith(key, "rule"))
        {
            const auto & handler_type = server.config().getString(prefix + "." + key + ".handler.type", "");

            if (handler_type.empty())
                throw Exception("Handler type in config is not specified here: " + prefix + "." + key + ".handler.type",
                    ErrorCodes::INVALID_CONFIG_PARAMETER);

            if (handler_type == "static")
                main_handler_factory->addHandler(createStaticHandlerFactory(server, prefix + "." + key));
            else if (handler_type == "dynamic_query_handler")
                main_handler_factory->addHandler(createDynamicHandlerFactory(server, prefix + "." + key));
            else if (handler_type == "predefined_query_handler")
                main_handler_factory->addHandler(createPredefinedHandlerFactory(server, prefix + "." + key));
            else if (handler_type == "prometheus")
                main_handler_factory->addHandler(createPrometheusHandlerFactory(server, async_metrics, prefix + "." + key));
            else
                throw Exception("Unknown handler type '" + handler_type + "' in config here: " + prefix + "." + key + ".handler.type",
                    ErrorCodes::INVALID_CONFIG_PARAMETER);
        }
        else
            throw Exception("Unknown element in config: " + prefix + "." + key + ", must be 'rule' or 'defaults'",
                ErrorCodes::UNKNOWN_ELEMENT_IN_CONFIG);
    }

    return main_handler_factory;
}

static inline HTTPRequestHandlerFactoryPtr
/// proton: starts
createHTTPHandlerFactory(IServer & server, const std::string & name, AsynchronousMetrics & async_metrics, bool snapshot_mode_ = false)
/// proton: ends
{
    if (server.config().has("http_handlers"))
    {
        /// proton: starts
        return createHandlersFactoryFromConfig(server, name, "http_handlers", async_metrics, snapshot_mode_);
        /// proton: ends
    }
    else
    {
        auto factory = std::make_shared<HTTPRequestHandlerFactoryMain>(name);
        /// proton: starts
        addDefaultHandlersFactory(*factory, server, async_metrics, snapshot_mode_);
        /// proton: ends
        return factory;
    }
}

static inline HTTPRequestHandlerFactoryPtr createInterserverHTTPHandlerFactory(IServer & server, const std::string & name)
{
    auto factory = std::make_shared<HTTPRequestHandlerFactoryMain>(name);
    addCommonDefaultHandlersFactory(*factory, server);

    auto main_handler = std::make_shared<HandlingRuleHTTPHandlerFactory<InterserverIOHTTPHandler>>(server);
    main_handler->allowPostAndGetParamsAndOptionsRequest();
    factory->addHandler(main_handler);

    return factory;
}

namespace
{
void addPrometheusHandler(IServer & server, AsynchronousMetrics & async_metrics, HTTPRequestHandlerFactoryMain & factory)
{
    for (const auto & path : {std::string{"/timeplusd/metrics"}, server.config().getString("prometheus.endpoint", "/metrics")})
    {
        auto prometheus_handler = std::make_shared<HandlingRuleHTTPHandlerFactory<PrometheusRequestHandler>>(
            server, PrometheusMetricsWriter(server.config(), "prometheus", async_metrics, server.context()));
        prometheus_handler->attachStrictPath(path);
        prometheus_handler->allowGetAndHeadRequest();

        factory.addHandler(std::move(prometheus_handler));
    }
}
}

HTTPRequestHandlerFactoryPtr createHandlerFactory(IServer & server, AsynchronousMetrics & async_metrics, const std::string & name)
{
    if (name == "HTTPHandler-factory" || name == "HTTPSHandler-factory")
        return createHTTPHandlerFactory(server, name, async_metrics);
    /// proton: starts. turn on snapshot_mode
    else if (name == "SnapshotHTTPHandler-factory")
        return createHTTPHandlerFactory(server, name, async_metrics, true);
    /// proton: ends
    else if (name == "InterserverIOHTTPHandler-factory" || name == "InterserverIOHTTPSHandler-factory")
        return createInterserverHTTPHandlerFactory(server, name);
    else if (name == "PrometheusHandler-factory")
    {
        auto factory = std::make_shared<HTTPRequestHandlerFactoryMain>(name);
        addPrometheusHandler(server, async_metrics, *factory);
        return factory;
    }

    throw Exception("LOGICAL ERROR: Unknown HTTP handler factory name.", ErrorCodes::LOGICAL_ERROR);
}

/// proton: starts.
HTTPRequestHandlerFactoryPtr createMetaStoreHandlerFactory(IServer & server, const std::string & name)
{
    auto factory = std::make_shared<HTTPRequestHandlerFactoryMain>(name);
    for (const auto * prefix : {"timeplusd", "proton"})
    {
        auto rest_handler = std::make_shared<HandlingRuleHTTPHandlerFactory<RestHTTPRequestHandler>>(server, "metastore");
        rest_handler->attachNonStrictPath(fmt::format("/{}/metastore", prefix));
        factory->addHandler(rest_handler);
    }

    return factory;
}
/// proton: ends.

static const auto root_response_expression = "config://http_server_default_response";

void addCommonDefaultHandlersFactory(HTTPRequestHandlerFactoryMain & factory, IServer & server)
{
    auto root_handler = std::make_shared<HandlingRuleHTTPHandlerFactory<StaticRequestHandler>>(server, root_response_expression);
    root_handler->attachStrictPath("/");
    root_handler->allowGetAndHeadRequest();
    factory.addHandler(root_handler);

    auto web_ui_handler = std::make_shared<HandlingRuleHTTPHandlerFactory<WebUIRequestHandler>>(server, "play.html");
    web_ui_handler->attachNonStrictPath("/timeplusd/play");
    web_ui_handler->allowGetAndHeadRequest();
    factory.addHandler(web_ui_handler);
}

/// proton: starts
void addDefaultHandlersFactory(HTTPRequestHandlerFactoryMain & factory, IServer & server, AsynchronousMetrics & async_metrics, bool snapshot_mode_)
/// proton: ends
{
    addCommonDefaultHandlersFactory(factory, server);

    /// proton: start. Add rest request process handler
    {
        auto rest_handler = std::make_shared<HandlingRuleHTTPHandlerFactory<RestHTTPRequestHandler>>(server, "proton");
        rest_handler->attachNonStrictPath("/proton");
        factory.addHandler(rest_handler);
    }

    {
        auto rest_handler = std::make_shared<HandlingRuleHTTPHandlerFactory<RestHTTPRequestHandler>>(server, "timeplusd");
        rest_handler->attachNonStrictPath("/timeplusd");
        factory.addHandler(rest_handler);
    }
    /// proton: end.

    /// proton: starts
    auto query_handler = std::make_shared<HandlingRuleHTTPHandlerFactory<DynamicQueryHandler>>(server, "query", std::move(snapshot_mode_));
    /// proton: ends
    query_handler->allowPostAndGetParamsAndOptionsRequest();
    factory.addHandler(query_handler);

    /// We check that prometheus handler will be served on current (default) port.
    /// Otherwise it will be created separately, see createHandlerFactory(...).
    if (server.config().has("prometheus") && server.config().getInt("prometheus.port", 0) == 0)
        addPrometheusHandler(server, async_metrics, factory);
}

}
