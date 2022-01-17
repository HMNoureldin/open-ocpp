/*
Copyright (c) 2020 Cedric Jimenez
This file is part of OpenOCPP.

OpenOCPP is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

OpenOCPP is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License
along with OpenOCPP. If not, see <http://www.gnu.org/licenses/>.
*/

#include "TransactionManager.h"
#include "AuthentManager.h"
#include "Connectors.h"
#include "GenericMessageSender.h"
#include "IChargePointEventsHandler.h"
#include "IMeterValuesManager.h"
#include "IOcppConfig.h"
#include "ISmartChargingManager.h"
#include "IStatusManager.h"
#include "Logger.h"
#include "MeterValues.h"
#include "ReservationManager.h"
#include "StartTransaction.h"
#include "StopTransaction.h"
#include "WorkerThreadPool.h"

using namespace ocpp::types;
using namespace ocpp::messages;

namespace ocpp
{
namespace chargepoint
{

/** @brief Constructor */
TransactionManager::TransactionManager(ocpp::config::IOcppConfig&                      ocpp_config,
                                       IChargePointEventsHandler&                      events_handler,
                                       ocpp::helpers::TimerPool&                       timer_pool,
                                       ocpp::helpers::WorkerThreadPool&                worker_pool,
                                       ocpp::database::Database&                       database,
                                       Connectors&                                     connectors,
                                       const ocpp::messages::GenericMessagesConverter& messages_converter,
                                       ocpp::messages::IMessageDispatcher&             msg_dispatcher,
                                       ocpp::messages::GenericMessageSender&           msg_sender,
                                       IStatusManager&                                 status_manager,
                                       AuthentManager&                                 authent_manager,
                                       ReservationManager&                             reservation_manager,
                                       IMeterValuesManager&                            meter_values_manager,
                                       ISmartChargingManager&                          smart_charging_manager)
    : GenericMessageHandler<RemoteStartTransactionReq, RemoteStartTransactionConf>(REMOTE_START_TRANSACTION_ACTION, messages_converter),
      GenericMessageHandler<RemoteStopTransactionReq, RemoteStopTransactionConf>(REMOTE_STOP_TRANSACTION_ACTION, messages_converter),
      m_ocpp_config(ocpp_config),
      m_events_handler(events_handler),
      m_worker_pool(worker_pool),
      m_connectors(connectors),
      m_msg_sender(msg_sender),
      m_status_manager(status_manager),
      m_authent_manager(authent_manager),
      m_reservation_manager(reservation_manager),
      m_meter_values_manager(meter_values_manager),
      m_smart_charging_manager(smart_charging_manager),
      m_requests_fifo(database),
      m_request_retry_timer(timer_pool, "Transaction FIFO"),
      m_request_retry_count(0)
{
    msg_dispatcher.registerHandler(REMOTE_START_TRANSACTION_ACTION,
                                   *dynamic_cast<GenericMessageHandler<RemoteStartTransactionReq, RemoteStartTransactionConf>*>(this));
    msg_dispatcher.registerHandler(REMOTE_STOP_TRANSACTION_ACTION,
                                   *dynamic_cast<GenericMessageHandler<RemoteStopTransactionReq, RemoteStopTransactionConf>*>(this));
    m_meter_values_manager.setTransactionFifo(m_requests_fifo);
    m_request_retry_timer.setCallback([this] { m_worker_pool.run<void>(std::bind(&TransactionManager::processFifoRequest, this)); });
}

/** @brief Destructor */
TransactionManager::~TransactionManager() { }

/** @brief Update the charge point connection status */
void TransactionManager::updateConnectionStatus(bool is_connected)
{
    if (is_connected)
    {
        // Check if the FIFO must be emptied
        if (m_requests_fifo.size() != 0)
        {
            LOG_INFO << "Restart transaction related FIFO processing";

            // Start processing FIFO requests
            m_worker_pool.run<void>(std::bind(&TransactionManager::processFifoRequest, this));
        }
    }
}

/** @brief Start a transaction */
ocpp::types::AuthorizationStatus TransactionManager::startTransaction(unsigned int connector_id, const std::string& id_tag)
{
    AuthorizationStatus ret = AuthorizationStatus::Invalid;

    // Not allowed to start a transaction on connector 0
    if (connector_id != 0)
    {
        // Get requested connector
        Connector* connector = m_connectors.getConnector(connector_id);
        if (connector)
        {
            // Check if no pending reservation on this connector
            ret = m_reservation_manager.isTransactionAllowed(connector_id, id_tag);
            if (ret == AuthorizationStatus::Accepted)
            {
                // Prepare message
                StartTransactionReq start_transaction_req;
                start_transaction_req.connectorId = connector_id;
                start_transaction_req.idTag.assign(id_tag);
                start_transaction_req.meterStart = m_events_handler.getTxStartStopMeterValue(connector_id);
                start_transaction_req.timestamp  = DateTime::now();

                // Check reservations
                if (connector->status == ChargePointStatus::Reserved)
                {
                    // Fill reservation id
                    start_transaction_req.reservationId = connector->reservation_id;

                    // Clear reservation
                    m_reservation_manager.clearReservation(connector_id);
                }
                else
                {
                    // Check reservation on the whole charge point
                    if (m_ocpp_config.reserveConnectorZeroSupported())
                    {
                        Connector& charge_point = m_connectors.getChargePointConnector();
                        if (charge_point.status == ChargePointStatus::Reserved)
                        {
                            // Check if this transaction can be used for the charge point reservation
                            if (m_reservation_manager.isTransactionAllowed(Connectors::CONNECTOR_ID_CHARGE_POINT, id_tag) ==
                                AuthorizationStatus::Accepted)
                            {
                                // Fill reservation id
                                start_transaction_req.reservationId = charge_point.reservation_id;

                                // Clear reservation
                                m_reservation_manager.clearReservation(connector_id);
                            }
                        }
                    }
                }

                LOG_INFO << "Start transaction requested : connector = " << start_transaction_req.connectorId
                         << " - idTag = " << start_transaction_req.idTag.c_str();

                // Send message
                StartTransactionConf start_transaction_conf;
                CallResult           result =
                    m_msg_sender.call(START_TRANSACTION_ACTION, start_transaction_req, start_transaction_conf, &m_requests_fifo);
                if (result == CallResult::Ok)
                {
                    // Save response status
                    ret = start_transaction_conf.idTagInfo.status;

                    // Update id tag information
                    if (ret != AuthorizationStatus::ConcurrentTx)
                    {
                        m_authent_manager.update(id_tag, start_transaction_conf.idTagInfo);
                    }
                }
                else
                {
                    // Send the message later, authorize transaction meanwhile
                    start_transaction_conf.transactionId = -1;
                    ret                                  = AuthorizationStatus::Accepted;
                }
                if (ret == AuthorizationStatus::Accepted)
                {
                    LOG_INFO << "Start transaction accepted : connector = " << start_transaction_req.connectorId
                             << " - transactionId = " << start_transaction_conf.transactionId;

                    // Update status from response
                    {
                        std::lock_guard<std::mutex> lock(connector->mutex);
                        connector->transaction_id     = start_transaction_conf.transactionId;
                        connector->transaction_start  = DateTime::now();
                        connector->transaction_id_tag = id_tag;
                        m_connectors.saveConnector(connector->id);
                    }

                    // Assign pending charging profiles to the transaction
                    m_smart_charging_manager.assignPendingTxProfiles(connector_id, connector->transaction_id);

                    // Start sampled meter values on this connector
                    m_meter_values_manager.startSampledMeterValues(connector_id);
                }
                else
                {
                    LOG_WARNING << "Start transaction refused : connector = " << start_transaction_req.connectorId
                                << " - authorizationStatus = " << AuthorizationStatusHelper.toString(ret);

                    // Send a stop transaction to close the corresponding transaction id in the central system
                    // (required only for some central system implementations but cannot hurt on other since
                    // transactionId field must be unique)
                    StopTransactionReq stop_transaction_req;
                    stop_transaction_req.transactionId = start_transaction_conf.transactionId;
                    stop_transaction_req.timestamp     = start_transaction_req.timestamp;
                    stop_transaction_req.meterStop     = start_transaction_req.meterStart;
                    stop_transaction_req.reason        = Reason::DeAuthorized;
                    StopTransactionConf stop_transaction_conf;
                    m_msg_sender.call(STOP_TRANSACTION_ACTION, stop_transaction_req, stop_transaction_conf, &m_requests_fifo);
                }
            }
        }
    }

    return ret;
}

/** @brief Stop a transaction */
bool TransactionManager::stopTransaction(unsigned int connector_id, const std::string& id_tag, ocpp::types::Reason reason)
{
    bool ret = false;

    // Get requested connector
    Connector* connector = m_connectors.getConnector(connector_id);
    if (connector)
    {
        // Check if a transaction is in progress
        if (connector->transaction_id != 0)
        {
            // Stop sampled meter values on this connector
            m_meter_values_manager.stopSampledMeterValues(connector_id);

            // Stop transaction
            StopTransactionReq stop_transaction_req;
            if (!id_tag.empty())
            {
                stop_transaction_req.idTag.value().assign(id_tag);
            }
            stop_transaction_req.meterStop     = m_events_handler.getTxStartStopMeterValue(connector_id);
            stop_transaction_req.timestamp     = DateTime::now();
            stop_transaction_req.transactionId = connector->transaction_id;
            stop_transaction_req.reason        = reason;
            m_meter_values_manager.getTxStopMeterValues(connector_id, stop_transaction_req.transactionData);

            // Reset transaction id
            {
                std::lock_guard<std::mutex> lock(connector->mutex);
                connector->transaction_id     = 0;
                connector->transaction_id_tag = "";
                connector->transaction_start  = 0;
                m_connectors.saveConnector(connector->id);
            }

            LOG_INFO << "Stop transaction : transactionId = " << stop_transaction_req.transactionId
                     << " - idTag = " << (stop_transaction_req.idTag.isSet() ? stop_transaction_req.idTag.value().c_str() : "empty")
                     << " - reason = " << ReasonHelper.toString(stop_transaction_req.reason);

            // Send message
            StopTransactionConf stop_transaction_conf;
            CallResult result = m_msg_sender.call(STOP_TRANSACTION_ACTION, stop_transaction_req, stop_transaction_conf, &m_requests_fifo);
            if (result == CallResult::Ok)
            {
                // Update id tag information
                if (stop_transaction_conf.idTagInfo.isSet())
                {
                    m_authent_manager.update(id_tag, stop_transaction_conf.idTagInfo);
                }
            }

            // Remove charging profiles for this transaction
            m_smart_charging_manager.clearTxProfiles(connector_id);

            ret = true;
        }
    }

    return ret;
}

/** @copydoc bool GenericMessageHandler<RequestType, ResponseType>::handleMessage(const RequestType& request,
     *                                                                                ResponseType& response,
     *                                                                                const char*& error_code,
     *                                                                                std::string& error_message)
     */
bool TransactionManager::handleMessage(const ocpp::messages::RemoteStartTransactionReq& request,
                                       ocpp::messages::RemoteStartTransactionConf&      response,
                                       const char*&                                     error_code,
                                       std::string&                                     error_message)
{
    (void)error_code;
    (void)error_message;

    LOG_INFO << "Remote start transaction requested : connector = " << request.connectorId << " - idTag = " << request.idTag.c_str();

    // No remote start allowed without connector id
    bool authorized = false;
    if (request.connectorId.isSet() && (request.connectorId != Connectors::CONNECTOR_ID_CHARGE_POINT))
    {
        // Get requested connector
        Connector* connector = m_connectors.getConnector(request.connectorId);
        if (connector)
        {
            // Check if a transaction is in progress and if a transaction is allowed on this connector
            if ((connector->status != ChargePointStatus::Unavailable) && (connector->transaction_id == 0) &&
                (m_reservation_manager.isTransactionAllowed(request.connectorId, request.idTag.str()) == AuthorizationStatus::Accepted))
            {
                // Notify request
                authorized = m_events_handler.remoteStartTransactionRequested(request.connectorId, request.idTag.str());
                if (authorized && request.chargingProfile.isSet())
                {
                    // Install associated charging profile
                    authorized = m_smart_charging_manager.installTxProfile(request.connectorId, request.chargingProfile);
                }
            }
        }
    }

    // Response
    if (authorized)
    {
        response.status = RemoteStartStopStatus::Accepted;
    }
    else
    {
        response.status = RemoteStartStopStatus::Rejected;
    }

    LOG_INFO << "Remote start transaction " << RemoteStartStopStatusHelper.toString(response.status)
             << " : connector = " << request.connectorId;

    return true;
}

/** @copydoc bool GenericMessageHandler<RequestType, ResponseType>::handleMessage(const RequestType& request,
 *                                                                                ResponseType& response,
 *                                                                                const char*& error_code,
 *                                                                                std::string& error_message)
 */
bool TransactionManager::handleMessage(const ocpp::messages::RemoteStopTransactionReq& request,
                                       ocpp::messages::RemoteStopTransactionConf&      response,
                                       const char*&                                    error_code,
                                       std::string&                                    error_message)
{
    (void)error_code;
    (void)error_message;

    LOG_INFO << "Remote stop transaction requested : transactionId = " << request.transactionId;

    // Look for the requested transaction
    bool authorized = false;
    for (const Connector* connector : m_connectors.getConnectors())
    {
        if ((connector->transaction_id != 0) && (connector->transaction_id == request.transactionId))
        {
            // Notify request
            authorized = m_events_handler.remoteStopTransactionRequested(connector->id);
            break;
        }
    }

    // Response
    if (authorized)
    {
        response.status = RemoteStartStopStatus::Accepted;
    }
    else
    {
        response.status = RemoteStartStopStatus::Rejected;
    }

    LOG_INFO << "Remote stop transaction " << RemoteStartStopStatusHelper.toString(response.status)
             << " : transactionId = " << request.transactionId;

    return true;
}

/** @brief Process a FIFO request */
void TransactionManager::processFifoRequest()
{
    // Check the connection state
    if (m_msg_sender.isConnected())
    {
        // Check registration status
        if (m_status_manager.getRegistrationStatus() == RegistrationStatus::Accepted)
        {
            do
            {
                // Get request
                std::string         action;
                rapidjson::Document payload;
                if (m_requests_fifo.front(action, payload))
                {
                    LOG_DEBUG << "Request FIFO processing " << action << "retries : " << m_request_retry_count << "/"
                              << m_ocpp_config.transactionMessageAttempts();

                    // Send request
                    CallResult res;
                    if (action == START_TRANSACTION_ACTION)
                    {
                        // Start transaction => result contains validity information
                        StartTransactionConf response;
                        res = m_msg_sender.call(action, payload, response);
                        if (res == CallResult::Ok)
                        {
                            // Extract transaction from the request
                            StartTransactionReq          request;
                            StartTransactionReqConverter req_converter;
                            std::string                  error_message;
                            const char*                  error_code = nullptr;
                            req_converter.fromJson(payload, request, error_code, error_message);

                            // Update id tag information
                            if (response.idTagInfo.status != AuthorizationStatus::ConcurrentTx)
                            {
                                m_authent_manager.update(request.idTag, response.idTagInfo);
                            }

                            // Check if transaction has been rejected by the Central System
                            if (response.idTagInfo.status != AuthorizationStatus::Accepted)
                            {
                                // Look for the corresponding transaction
                                Connector* connector = m_connectors.getConnector(request.connectorId);
                                if (connector && (connector->transaction_id == -1) &&
                                    (connector->transaction_id_tag == request.idTag.str()))
                                {
                                    // Notify end of transaction
                                    m_events_handler.transactionDeAuthorized(connector->id);
                                }
                            }
                        }
                    }
                    else if (action == STOP_TRANSACTION_ACTION)
                    {
                        // Stop transaction => ignore response
                        StopTransactionConf response;
                        res = m_msg_sender.call(action, payload, response);
                    }
                    else if (action == METER_VALUES_ACTION)
                    {
                        // Meter values => ignore response
                        MeterValuesConf response;
                        res = m_msg_sender.call(action, payload, response);
                    }
                    else
                    {
                        // Unknown action
                        res = CallResult::Failed;
                    }
                    if (res == CallResult::Ok)
                    {
                        LOG_DEBUG << "Request succeeded";

                        // Remove request from the FIFO
                        m_requests_fifo.pop();
                        m_request_retry_count = 0;
                    }
                    else
                    {
                        // Update retry count
                        m_request_retry_count++;
                        if (m_request_retry_count > m_ocpp_config.transactionMessageAttempts())
                        {
                            // Drop message from the FIFO
                            LOG_DEBUG << "Request failed, drop message";
                            m_requests_fifo.pop();
                            m_request_retry_count = 0;
                        }
                        else
                        {
                            // Schedule next retry
                            if (m_msg_sender.isConnected())
                            {
                                LOG_DEBUG << "Request failed, next retry in " << m_ocpp_config.transactionMessageRetryInterval().count()
                                          << "second(s)";
                                m_request_retry_timer.restart(std::chrono::seconds(m_ocpp_config.transactionMessageRetryInterval()), true);
                            }
                        }
                    }
                }
            } while ((m_requests_fifo.size() != 0) && !m_request_retry_timer.isStarted() && m_msg_sender.isConnected());
        }
        else
        {
            // Wait to be accepted by the Central System
            m_request_retry_timer.restart(std::chrono::milliseconds(250u), true);
        }
    }
}

} // namespace chargepoint
} // namespace ocpp