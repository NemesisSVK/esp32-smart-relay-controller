#ifndef WEB_REQUEST_ADAPTER_H
#define WEB_REQUEST_ADAPTER_H

#include <Arduino.h>
#include <FS.h>
#include <functional>
#include <memory>
#include "MainController.h"

namespace webserver {

enum class RequestMethod : uint8_t {
    Any = 0,
    Get,
    Post,
    Put,
    Patch,
    Delete,
    Options,
    Head,
    Unknown
};

constexpr size_t kContentLengthUnknown = static_cast<size_t>(-1);

// Thin adapter interfaces to decouple handler logic from a specific HTTP server backend.
class IRequest {
public:
    virtual ~IRequest() = default;

    virtual RequestMethod method() = 0;
    virtual String uri() = 0;
    virtual bool hasArg(const String& name) = 0;
    virtual String arg(const String& name) = 0;
    virtual String body() = 0;
    virtual IPAddress remoteIP() = 0;
};

class IResponse {
public:
    virtual ~IResponse() = default;

    virtual void send(int statusCode, const String& contentType, const String& body) = 0;
    virtual void send(int statusCode) = 0;
    virtual void sendHeader(const String& name, const String& value) = 0;
    virtual size_t streamFile(File& file, const String& contentType) = 0;
    virtual size_t sendFile(FS& fs, const char* path, const String& contentType) = 0;
    virtual void setContentLength(size_t contentLength) = 0;
    virtual void sendContent(const String& content) = 0;
    virtual bool authenticate(const char* username, const char* password) = 0;
    virtual void requestAuthentication() = 0;
};

using ExchangeHandler = std::function<void(IRequest&, IResponse&)>;

enum class WebServerBackendKind : uint8_t {
    Async = 0
};

class IWebServerBackend {
public:
    virtual ~IWebServerBackend() = default;

    virtual WebServerBackendKind kind() const = 0;
    virtual void begin() = 0;
    virtual void poll() = 0;
    virtual void registerRoute(const char* uri, RequestMethod method, const ExchangeHandler& handler) = 0;
};

std::unique_ptr<IWebServerBackend> createWebServerBackend(WebServerBackendKind kind, uint16_t port);
const char* webServerBackendKindName(WebServerBackendKind kind);

enum class ControlDispatchWaitStatus : uint8_t {
    ControllerUnavailable = 0,
    EnqueueRejected,
    Completed,
    TimedOut,
    ResultUnavailable
};

struct ControlDispatchWaitResult {
    ControlDispatchWaitStatus status = ControlDispatchWaitStatus::ControllerUnavailable;
    uint32_t commandId = 0;
    MainController::ControlCommandResult commandResult;
};

ControlDispatchWaitResult dispatchControlCommandAndWait(
    MainController* mainController,
    const MainController::ControlCommand& command,
    unsigned long timeoutMs = 1000UL,
    unsigned long pollIntervalMs = 1UL);

ControlDispatchWaitResult dispatchSettingsUpdateAndWait(
    MainController* mainController,
    const String& jsonBody,
    unsigned long timeoutMs = 15000UL,
    unsigned long pollIntervalMs = 1UL);

} // namespace webserver

#endif // WEB_REQUEST_ADAPTER_H
