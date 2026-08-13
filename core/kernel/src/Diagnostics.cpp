#include "cad/kernel/Diagnostics.h"

#include "cad/log/Log.h"

#include <Message.hxx>
#include <Message_Gravity.hxx>
#include <Message_Messenger.hxx>
#include <Message_Printer.hxx>
#include <TCollection_AsciiString.hxx>

namespace cad::kernel {
namespace {

/// Forwards OCCT messages to our log, mapping its gravity onto our levels.
class LogPrinter final : public Message_Printer {
public:
    void send(const TCollection_AsciiString& text, const Message_Gravity gravity) const override {
        if (text.IsEmpty()) return;
        // OCCT is chatty at Info and below during healing and import, so those land at Debug: on
        // by request, never by default. Alarm and Fail become warnings and errors, because they
        // are the ones that explain a failure the user is about to see.
        const char* message = text.ToCString();
        switch (gravity) {
            case Message_Fail:
                CAD_ERROR(log::Category::ThirdParty) << "OCCT: " << message;
                break;
            case Message_Alarm:
                CAD_WARN(log::Category::ThirdParty) << "OCCT: " << message;
                break;
            case Message_Warning:
                CAD_WARN(log::Category::ThirdParty) << "OCCT: " << message;
                break;
            default:
                CAD_DEBUG(log::Category::ThirdParty) << "OCCT: " << message;
                break;
        }
    }
};

}  // namespace

void routeOcctDiagnosticsToLog() {
    const Handle(Message_Messenger) messenger = Message::DefaultMessenger();
    if (messenger.IsNull()) return;
    // REMOVE the default printers rather than adding alongside them. Leaving OCCT's std::cout
    // printer in place would duplicate every line -- once to the terminal unformatted, once
    // through the log -- which is worse than either alone.
    messenger->RemovePrinters(STANDARD_TYPE(Message_Printer));
    messenger->AddPrinter(new LogPrinter());
}

}  // namespace cad::kernel
