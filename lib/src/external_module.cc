#include <pxp-agent/external_module.hpp>

#define LEATHERMAN_LOGGING_NAMESPACE "puppetlabs.pxp_agent.external_module"
#include <leatherman/logging/logging.hpp>
#include <leatherman/execution/execution.hpp>
#include <leatherman/file_util/file.hpp>

#include <horsewhisperer/horsewhisperer.h>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include <atomic>
#include <memory>  // shared_ptr

// TODO(ale): disable assert() once we're confident with the code...
// To disable assert()
// #define NDEBUG
#include <cassert>

namespace PXPAgent {

static const std::string METADATA_SCHEMA_NAME { "external_module_metadata" };
static const std::string ACTION_SCHEMA_NAME { "action_metadata" };

static const std::string METADATA_CONFIGURATION_ENTRY { "configuration" };
static const std::string METADATA_ACTIONS_ENTRY { "actions" };

static const int EXTERNAL_MODULE_FILE_ERROR_EC { 5 };

namespace fs = boost::filesystem;
namespace HW = HorseWhisperer;
namespace lth_exec = leatherman::execution;
namespace lth_file = leatherman::file_util;

//
// Free functions
//

// Provides the module metadata validator
PCPClient::Validator getMetadataValidator() {
    // Metadata schema
    PCPClient::Schema metadata_schema { METADATA_SCHEMA_NAME,
                                          PCPClient::ContentType::Json };
    using T_C = PCPClient::TypeConstraint;
    metadata_schema.addConstraint("description", T_C::String, true);
    metadata_schema.addConstraint(METADATA_CONFIGURATION_ENTRY, T_C::Object, false);
    metadata_schema.addConstraint(METADATA_ACTIONS_ENTRY, T_C::Array, true);

    // 'actions' is an array of actions; define the action sub_schema
    PCPClient::Schema action_schema { ACTION_SCHEMA_NAME,
                                      PCPClient::ContentType::Json };
    action_schema.addConstraint("description", T_C::String, false);
    action_schema.addConstraint("name", T_C::String, true);
    action_schema.addConstraint("input", T_C::Object, true);
    action_schema.addConstraint("results", T_C::Object, true);

    metadata_schema.addConstraint(METADATA_ACTIONS_ENTRY, action_schema, false);

    PCPClient::Validator validator {};
    validator.registerSchema(metadata_schema);
    return validator;
}

//
// Public interface
//

ExternalModule::ExternalModule(const std::string& path,
                               const lth_jc::JsonContainer& config)
        : path_ { path },
          config_ { config } {
    fs::path module_path { path };
    module_name = module_path.stem().string();
    auto metadata = getMetadata();

    try {
        if (metadata.includes(METADATA_CONFIGURATION_ENTRY)) {
            registerConfiguration(
                metadata.get<lth_jc::JsonContainer>(METADATA_CONFIGURATION_ENTRY));
        } else {
            LOG_DEBUG("Found no configuration schema for module '%1%'", module_name);
        }

        registerActions(metadata);
    } catch (lth_jc::data_error& e) {
        LOG_ERROR("Failed to retrieve metadata of module %1%: %2%",
                  module_name, e.what());
        std::string err { "invalid metadata of module " + module_name };
        throw Module::LoadingError { err };
    }
}

ExternalModule::ExternalModule(const std::string& path)
        : path_ { path },
          config_ { "{}" } {
    fs::path module_path { path };
    module_name = module_path.stem().string();
    auto metadata = getMetadata();

    try {
       registerActions(metadata);
    } catch (lth_jc::data_error& e) {
        LOG_ERROR("Failed to retrieve metadata of module %1%: %2%",
                  module_name, e.what());
        std::string err { "invalid metadata of module " + module_name };
        throw Module::LoadingError { err };
    }
}

void ExternalModule::validateConfiguration() {
    if (config_validator_.includesSchema(module_name)) {
        config_validator_.validate(config_, module_name);
    } else {
        LOG_DEBUG("The '%1%' configuration will not be validated; no JSON "
                  "schema is available", module_name);
    }
}

//
// Static functions
//

void ExternalModule::readNonBlockingOutcome(const ActionRequest& request,
                                            const std::string& out_file,
                                            const std::string& err_file,
                                            std::string& out_txt,
                                            std::string& err_txt) {
    if (fs::exists(err_file)) {
        if (!lth_file::read(err_file, err_txt)) {
            LOG_ERROR("Failed to read error file '%1%' of '%2% %3%'; will "
                      "continue processing the output",
                      err_file, request.module(), request.action());
        } else {
            LOG_TRACE("Successfully read error file '%1%'", err_file);
        }
    }

    if (!fs::exists(out_file)) {
        LOG_DEBUG("Output file '%1%' of '%2% %3%' does not exist",
                  out_file, request.module(), request.action());
    } else if (!lth_file::read(out_file, out_txt)) {
        LOG_ERROR("Failed to read output file '%1%' of '%2% %3%'",
                  out_file, request.module(), request.action());
        throw Module::ProcessingError { "failed to read" };
    } else if (out_txt.empty()) {
        LOG_TRACE("Output file '%1%' of '%2% %3%' is empty",
                  out_file, request.module(), request.action());
    } else {
        LOG_TRACE("Successfully read output file '%1%'", out_file);
    }
}

//
// Private interface
//

// Metadata validator (static member)
const PCPClient::Validator ExternalModule::metadata_validator_ {
        getMetadataValidator() };

// Retrieve and validate the module metadata
const lth_jc::JsonContainer ExternalModule::getMetadata() {
    auto exec =
#ifdef _WIN32
        lth_exec::execute("cmd.exe", { "/c", path_, "metadata" },
#else
        lth_exec::execute(path_, { "metadata" },
#endif
            0, {lth_exec::execution_options::merge_environment});

    if (!exec.error.empty()) {
        LOG_ERROR("Failed to load the external module metadata from %1%: %2%",
                  path_, exec.error);
        throw Module::LoadingError { "failed to load external module metadata" };
    }

    lth_jc::JsonContainer metadata;

    try {
        metadata = lth_jc::JsonContainer { exec.output };
        LOG_DEBUG("External module %1%: metadata is valid JSON", module_name);
    } catch (lth_jc::data_error& e) {
        throw Module::LoadingError { std::string { "metadata is not in a valid "
                                        "JSON format: " } + e.what() };
    }

    try {
        metadata_validator_.validate(metadata, METADATA_SCHEMA_NAME);
        LOG_DEBUG("External module %1%: metadata validation OK", module_name);
    } catch (PCPClient::validation_error& e) {
        throw Module::LoadingError { std::string { "metadata validation failure: " }
                                     + e.what() };
    }

    return metadata;
}

void ExternalModule::registerConfiguration(const lth_jc::JsonContainer& config_metadata) {
    try {
        PCPClient::Schema configuration_schema { module_name, config_metadata };
        LOG_DEBUG("Registering module config schema for '%1%'", module_name);
        config_validator_.registerSchema(configuration_schema);
    } catch (PCPClient::schema_error& e) {
        LOG_ERROR("Failed to parse the configuration schema of module '%1%': %2%",
                  module_name, e.what());
        std::string err { "invalid configuration schema of module " + module_name };
        throw Module::LoadingError { err };
    }
}

void ExternalModule::registerActions(const lth_jc::JsonContainer& metadata) {
    for (auto& action : metadata.get<std::vector<lth_jc::JsonContainer>>(
                                    METADATA_ACTIONS_ENTRY)) {
        registerAction(action);
    }
}

// Register the specified action after ensuring that the input and
// output schemas are valid JSON (i.e. we can instantiate Schema).
void ExternalModule::registerAction(const lth_jc::JsonContainer& action) {
    // NOTE(ale): name, input, and output are required action entries
    auto action_name = action.get<std::string>("name");
    LOG_DEBUG("Validating action '%1% %2%'", module_name, action_name);

    try {
        auto input_schema_json = action.get<lth_jc::JsonContainer>("input");
        PCPClient::Schema input_schema { action_name, input_schema_json };

        auto results_schema_json = action.get<lth_jc::JsonContainer>("results");
        PCPClient::Schema results_schema { action_name, results_schema_json };

        // Metadata schemas are valid JSON; store metadata
        LOG_DEBUG("Action '%1% %2%' has been validated", module_name, action_name);
        actions.push_back(action_name);
        input_validator_.registerSchema(input_schema);
        results_validator_.registerSchema(results_schema);
    } catch (PCPClient::schema_error& e) {
        LOG_ERROR("Failed to parse metadata schemas of action '%1% %2%': %3%",
                  module_name, action_name, e.what());
        std::string err { "invalid schemas of '" + module_name + " "
                          + action_name + "'" };
        throw Module::LoadingError { err };
    } catch (lth_jc::data_error& e) {
        LOG_ERROR("Failed to retrieve metadata schemas of action '%1% %2%': %3%",
                  module_name, action_name, e.what());
        std::string err { "invalid metadata of '" + module_name + " "
                          + action_name + "'" };
        throw Module::LoadingError { err };
    }
}

std::string ExternalModule::getActionArguments(const ActionRequest& request) {
    lth_jc::JsonContainer action_args {};
    action_args.set<lth_jc::JsonContainer>("input", request.params());

    if (!config_.empty())
        action_args.set<lth_jc::JsonContainer>("configuration", config_);

    if (request.type() == RequestType::NonBlocking) {
        fs::path r_d_p { request.resultsDir() };
        lth_jc::JsonContainer output_files {};
        output_files.set<std::string>("stdout", (r_d_p / "stdout").string());
        output_files.set<std::string>("stderr", (r_d_p / "stderr").string());
        output_files.set<std::string>("exitcode", (r_d_p / "exitcode").string());
        action_args.set<lth_jc::JsonContainer>("output_files", output_files);
    }

    return action_args.toString();
}

ActionOutcome ExternalModule::processRequestOutcome(const ActionRequest& request,
                                                    int exit_code,
                                                    std::string& out_txt,
                                                    std::string& err_txt) {
    if (out_txt.empty()) {
        LOG_TRACE("Obtained no results on stdout for the %1%",
                  request.prettyLabel());
    } else {
        LOG_TRACE("Results on stdout for the %1%: %2%",
                  request.prettyLabel(), out_txt);
    }

    if (exit_code != EXIT_SUCCESS) {
        LOG_TRACE("Execution failure (exit code %1%) for the %2%%3%",
                  exit_code, request.prettyLabel(),
                  (err_txt.empty() ? "" : "; stderr:\n" + err_txt));
    } else if (!err_txt.empty()) {
        LOG_TRACE("Output on stderr for the %1%:\n%2%",
                  request.prettyLabel(), err_txt);
    }

    try {
        // Ensure output format is valid JSON by instantiating JsonContainer
        // NB: JsonContainer's ctor does not accept empty strings
        lth_jc::JsonContainer results { (out_txt.empty() ? "null" : out_txt) };
        return ActionOutcome { exit_code, err_txt, out_txt, results };
    } catch (lth_jc::data_parse_error& e) {
        boost::format err_format;

        if (out_txt.empty()) {
            LOG_DEBUG("Obtained no output on stdout for the %1%",
                      request.prettyLabel());
            err_format = boost::format { "The task executed for the %1% returned "
                                         "no output on stdout - stderr:%2%" };
        } else {
            LOG_DEBUG("Obtained invalid JSON on stdout for the %1%; "
                      "(validation error: %2%); stdout:\n%3%",
                      request.prettyLabel(), e.what(), out_txt);
            err_format = boost::format { "The task executed for the %1% returned "
                                         "invalid JSON on stdout - stderr:%2%" };
        }
        auto err_msg = (err_format % request.prettyLabel()
                                   % (err_txt.empty() ? " (empty)" : '\n' + err_txt))
                        .str();
        throw Module::ProcessingError { err_msg };
    }
}

ActionOutcome ExternalModule::callBlockingAction(const ActionRequest& request) {
    auto action_name = request.action();
    auto action_args = getActionArguments(request);

    LOG_INFO("Executing the %1%", request.prettyLabel());
    LOG_TRACE("Input for the %1%: %2%", request.prettyLabel(), action_args);

    auto exec = lth_exec::execute(
#ifdef _WIN32
        "cmd.exe", { "/c", path_, action_name },
#else
        path_, { action_name },
#endif
        action_args,                           // args
        std::map<std::string, std::string>(),  // environment
        0,                                     // timeout
        { lth_exec::execution_options::merge_environment });  // options

    return processRequestOutcome(request, exec.exit_code, exec.output, exec.error);
}

ActionOutcome ExternalModule::callNonBlockingAction(const ActionRequest& request) {
    auto action_name = request.action();
    auto input_txt = getActionArguments(request);
    fs::path results_dir_path { request.resultsDir() };
    auto out_file = (results_dir_path / "stdout").string();
    auto err_file = (results_dir_path / "stderr").string();

    LOG_INFO("Starting a task for the %1%; stdout and stderr will be stored in %2%",
             request.prettyLabel(), request.resultsDir());
    LOG_TRACE("Input for the %1%: %2%", request.prettyLabel(), input_txt);

    auto exec = lth_exec::execute(
#ifdef _WIN32
        "cmd.exe", { "/c", path_, action_name },
#else
        path_, { action_name },
#endif
        input_txt,  // input
        std::map<std::string, std::string>(),  // environment
        [results_dir_path](size_t pid) {
            auto pid_file = (results_dir_path / "pid").string();
            lth_file::atomic_write_to_file(std::to_string(pid) + "\n", pid_file);
        },          // pid callback
        0,          // timeout
        { lth_exec::execution_options::merge_environment });  // options

    if (exec.exit_code == EXTERNAL_MODULE_FILE_ERROR_EC) {
        // This is unexpected. The outcome of the task will not be
        // available for future transaction status requests
        LOG_WARNING("The task process failed to write output on file for the %1%; "
                    "stdout: %2%; stderr: %3%",
                    request.prettyLabel(),
                    (exec.output.empty() ? "(empty)" : exec.output),
                    (exec.error.empty() ? "(empty)" : exec.error));
        throw Module::ProcessingError { "failed to write output on file" };
    }

    // Stdout / stderr output is on file; read it
    std::string out_txt;
    std::string err_txt;
    readNonBlockingOutcome(request, out_file, err_file, out_txt, err_txt);

    return processRequestOutcome(request, exec.exit_code, out_txt, err_txt);
}

ActionOutcome ExternalModule::callAction(const ActionRequest& request) {
    if (request.type() == RequestType::Blocking) {
        return callBlockingAction(request);
    } else {
        // Guranteed by Configuration
        assert(!request.resultsDir().empty());
        return callNonBlockingAction(request);
    }
}

}  // namespace PXPAgent
