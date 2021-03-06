#include <pxp-agent/pxp_connector.hpp>
#include <pxp-agent/pxp_schemas.hpp>

#include <cpp-pcp-client/protocol/schemas.hpp>

#include <leatherman/util/strings.hpp>

#define LEATHERMAN_LOGGING_NAMESPACE "puppetlabs.pxp_agent.pxp_connector"
#include <leatherman/logging/logging.hpp>

namespace PXPAgent {

namespace lth_jc = leatherman::json_container;
namespace lth_util = leatherman::util;

static const int DEFAULT_MSG_TIMEOUT_SEC { 2 };

std::vector<lth_jc::JsonContainer> wrapDebug(
        const PCPClient::ParsedChunks& parsed_chunks) {
    auto request_id = parsed_chunks.envelope.get<std::string>("id");
    if (parsed_chunks.num_invalid_debug) {
        LOG_WARNING("Message %1% contained %2% bad debug chunk%3%",
                    request_id, parsed_chunks.num_invalid_debug,
                    lth_util::plural(parsed_chunks.num_invalid_debug));
    }
    std::vector<lth_jc::JsonContainer> debug {};
    for (auto& debug_entry : parsed_chunks.debug) {
        debug.push_back(debug_entry);
    }
    return debug;
}

PXPConnector::PXPConnector(const Configuration::Agent& agent_configuration)
        : PCPClient::Connector { agent_configuration.broker_ws_uri,
                                 agent_configuration.client_type,
                                 agent_configuration.ca,
                                 agent_configuration.crt,
                                 agent_configuration.key,
                                 agent_configuration.connection_timeout} {
}

void PXPConnector::sendPCPError(const std::string& request_id,
                                const std::string& description,
                                const std::vector<std::string>& endpoints) {
    lth_jc::JsonContainer pcp_error_data {};
    pcp_error_data.set<std::string>("id", request_id);
    pcp_error_data.set<std::string>("description", description);

    try {
        send(endpoints,
             PCPClient::Protocol::ERROR_MSG_TYPE,
             DEFAULT_MSG_TIMEOUT_SEC,
             pcp_error_data);
        LOG_INFO("Replied to request %1% with a PCP error message",
                 request_id);
    } catch (PCPClient::connection_error& e) {
        LOG_ERROR("Failed to send PCP error message for request %1%: %3%",
                  request_id, e.what());
    }
}

void PXPConnector::sendPXPError(const ActionRequest& request,
                                const std::string& description) {
    lth_jc::JsonContainer pxp_error_data {};
    pxp_error_data.set<std::string>("transaction_id", request.transactionId());
    pxp_error_data.set<std::string>("id", request.id());
    pxp_error_data.set<std::string>("description", description);

    try {
        send(std::vector<std::string> { request.sender() },
             PXPSchemas::PXP_ERROR_MSG_TYPE,
             DEFAULT_MSG_TIMEOUT_SEC,
             pxp_error_data);
        LOG_INFO("Replied to %1% by %2%, request ID %3%, with a PXP error message",
                 request.prettyLabel(), request.sender(), request.id());
    } catch (PCPClient::connection_error& e) {
        LOG_ERROR("Failed to send a PXP error message for the %1% by %2% "
                  "(no further sending attempts will be made): %3%",
                  request.prettyLabel(), request.sender(), description);
    }
}

void PXPConnector::sendBlockingResponse(const ActionRequest& request,
                                        const lth_jc::JsonContainer& results) {
    auto debug = wrapDebug(request.parsedChunks());
    lth_jc::JsonContainer response_data {};
    response_data.set<std::string>("transaction_id", request.transactionId());
    response_data.set<lth_jc::JsonContainer>("results", results);

    try {
        send(std::vector<std::string> { request.sender() },
             PXPSchemas::BLOCKING_RESPONSE_TYPE,
             DEFAULT_MSG_TIMEOUT_SEC,
             response_data,
             debug);
        LOG_INFO("Sent response for the %1% by %2%",
                 request.prettyLabel(), request.sender());
    } catch (PCPClient::connection_error& e) {
        LOG_ERROR("Failed to reply to the %1% by %2%: %3%",
                  request.prettyLabel(), request.sender(), e.what());
    }
}

void PXPConnector::sendNonBlockingResponse(const ActionRequest& request,
                                           const lth_jc::JsonContainer& results,
                                           const std::string& job_id) {
    lth_jc::JsonContainer response_data {};
    response_data.set<std::string>("transaction_id", request.transactionId());
    response_data.set<std::string>("job_id", job_id);
    response_data.set<lth_jc::JsonContainer>("results", results);

    try {
        // NOTE(ale): assuming debug was sent in provisional response
        send(std::vector<std::string> { request.sender() },
             PXPSchemas::NON_BLOCKING_RESPONSE_TYPE,
             DEFAULT_MSG_TIMEOUT_SEC,
             response_data);
        LOG_INFO("Sent response for the %1% by %2%",
                 request.prettyLabel(), request.sender());
    } catch (PCPClient::connection_error& e) {
        LOG_ERROR("Failed to reply to %1% by %2%, (no further attempts will "
                  "be made): %3%",
                  request.prettyLabel(), request.sender(), e.what());
    }
}

void PXPConnector::sendProvisionalResponse(const ActionRequest& request) {
    auto debug = wrapDebug(request.parsedChunks());
    lth_jc::JsonContainer provisional_data {};
    provisional_data.set<std::string>("transaction_id", request.transactionId());

    try {
        send(std::vector<std::string> { request.sender() },
             PXPSchemas::PROVISIONAL_RESPONSE_TYPE,
             DEFAULT_MSG_TIMEOUT_SEC,
             provisional_data,
             debug);
        LOG_INFO("Sent provisional response for the %1% by %2%",
                 request.prettyLabel(), request.sender());
    } catch (PCPClient::connection_error& e) {
        LOG_ERROR("Failed to send provisional response for the %1% by %2% "
                  "(no further attempts will be made): %3%",
                  request.prettyLabel(), request.sender(), e.what());
    }
}

}  // namesapce PXPAgent
