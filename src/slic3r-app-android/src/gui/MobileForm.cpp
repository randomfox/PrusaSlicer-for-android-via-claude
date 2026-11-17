#include "MobileHost.hpp"

#include <nlohmann/json.hpp>

namespace Slic3r::App::Mobile {

namespace {
const char* type_name(MobileForm::Field::Type type)
{
    switch (type) {
    case MobileForm::Field::Type::Text: return "text";
    case MobileForm::Field::Type::Number: return "number";
    case MobileForm::Field::Type::Choice: return "choice";
    case MobileForm::Field::Type::Check: return "check";
    case MobileForm::Field::Type::Info: return "info";
    }
    return "text";
}
} // namespace

std::string form_to_json(const MobileForm& form)
{
    nlohmann::json fields = nlohmann::json::array();
    for (const MobileForm::Field& field : form.fields) {
        fields.push_back({
            {"id", field.id},
            {"type", type_name(field.type)},
            {"label", field.label},
            {"value", field.value},
            {"options", field.options},
        });
    }
    const nlohmann::json json{
        {"title", form.title},
        {"text", form.text},
        {"error", form.error},
        {"fields", fields},
        {"buttons", form.buttons},
    };
    // Invalid UTF-8 (e.g. from a file name) must not throw.
    return json.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

MobileFormResult form_result_from_json(const std::string& text)
{
    MobileFormResult result;
    const nlohmann::json json = nlohmann::json::parse(text, nullptr, false);
    if (!json.is_object()) {
        return result;
    }
    result.button = json.value("button", -1);
    if (auto it = json.find("values"); it != json.end() && it->is_object()) {
        for (const auto& [key, value] : it->items()) {
            result.values[key] = value.is_string() ? value.get<std::string>() : value.dump();
        }
    }
    return result;
}

} // namespace Slic3r::App::Mobile
