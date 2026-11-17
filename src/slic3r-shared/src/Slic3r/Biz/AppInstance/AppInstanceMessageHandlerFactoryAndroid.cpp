// Android: an app has exactly one process instance, managed by the OS. There is nothing
// to communicate with, so the instance messaging is a no-op.

#include "Slic3r/Biz/AppInstance/AppInstanceMessageHandlerFactory.hpp"

namespace Slic3r::Biz::AppInstance {

namespace {

class AppInstanceMessageSenderAndroid : public AbstractAppInstanceMessageSender
{
public:
    void multicast_message(const std::string&, const std::string&, size_t, void*) override {}
    void broadcast_message(const std::string&, const std::string&, size_t, void*) override {}
};

class AppInstanceMessageHandlerAndroid : public AbstractAppInstanceMessageHandler
{
public:
    using AbstractAppInstanceMessageHandler::AbstractAppInstanceMessageHandler;

    void init(void*) override {}
    void multicast_message(const std::string&, const std::string&) override {}
    void on_becoming_primary_instance() override {}
};

} // namespace

std::unique_ptr<AbstractAppInstanceMessageSender> create_app_instance_message_sender()
{
    return std::make_unique<AppInstanceMessageSenderAndroid>();
}

std::unique_ptr<AbstractAppInstanceMessageHandler> create_app_instance_message_handler(
    Platform::IMainThreadDispatcher& dispatcher
)
{
    return std::make_unique<AppInstanceMessageHandlerAndroid>(dispatcher);
}

} // namespace Slic3r::Biz::AppInstance
