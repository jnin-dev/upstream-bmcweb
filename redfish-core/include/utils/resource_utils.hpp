// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright OpenBMC Authors
#pragma once

#include "async_resp.hpp"
#include "dbus_utility.hpp"
#include "error_messages.hpp"
#include "generated/enums/resource.hpp"
#include "logging.hpp"

#include <asm-generic/errno.h>

#include <boost/system/error_code.hpp>
#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <string>

namespace redfish
{
namespace resource_utils
{

struct ResourceStatus
{
    std::optional<bool> present;
    std::optional<bool> available;
    std::optional<bool> functional;
};

/**
 * @brief Fetches resource status.health from DBus interfaces
 *
 */
inline void determineResourceHealth(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const nlohmann::json::json_pointer& jsonPtr, bool functional)
{
    BMCWEB_LOG_DEBUG("determineResourceHealth");

    if (!functional)
    {
        asyncResp->res.jsonValue[jsonPtr]["Status"]["Health"] =
            resource::Health::Critical;
    }
    else
    {
        asyncResp->res.jsonValue[jsonPtr]["Status"]["Health"] =
            resource::Health::OK;
    }
}

inline void determineResourceState(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp, bool present,
    bool available, const nlohmann::json::json_pointer& jsonPtr)
{
    BMCWEB_LOG_DEBUG("determineResourceState");

    // Absent takes priority over unavailable
    if (!present)
    {
        asyncResp->res.jsonValue[jsonPtr]["Status"]["State"] =
            resource::State::Absent;
    }
    else if (!available)
    {
        asyncResp->res.jsonValue[jsonPtr]["Status"]["State"] =
            resource::State::UnavailableOffline;
    }
    else
    {
        asyncResp->res.jsonValue[jsonPtr]["Status"]["State"] =
            resource::State::Enabled;
    }
}

inline void getStatusAvailableState(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const nlohmann::json::json_pointer& jsonPtr, bool present,
    const boost::system::error_code& ec, bool available)
{
    if (ec)
    {
        if (ec.value() != EBADR)
        {
            BMCWEB_LOG_ERROR("DBUS response error for {}, ec {}", "Available",
                             ec.value());
            messages::internalError(asyncResp->res);
            return;
        }
        available = true;
    }
    determineResourceState(asyncResp, present, available, jsonPtr);
}

inline void getStatusPresentState(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& service, const std::string& path,
    const nlohmann::json::json_pointer& jsonPtr,
    const boost::system::error_code& ec, bool present)
{
    if (ec)
    {
        if (ec.value() != EBADR)
        {
            BMCWEB_LOG_ERROR("DBUS response error for {}, ec {}", "Present",
                             ec.value());
            messages::internalError(asyncResp->res);
            return;
        }
        // Interface is missing, default to true for Enabled
        present = true;
    }
    dbus::utility::getProperty<bool>(
        *crow::connections::systemBus, service, path,
        "xyz.openbmc_project.State.Decorator.Availability", "Available",
        std::bind_front(getStatusAvailableState, asyncResp, jsonPtr, present));
}

inline void getResourceState(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& service, const std::string& path,
    const nlohmann::json::json_pointer& jsonPtr)
{
    BMCWEB_LOG_DEBUG("getResourceStatus");
    dbus::utility::getProperty<bool>(
        *crow::connections::systemBus, service, path,
        "xyz.openbmc_project.Inventory.Item", "Present",
        std::bind_front(getStatusPresentState, asyncResp, service, path,
                        jsonPtr));
}

inline void determineResourceState(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::shared_ptr<ResourceStatus>& status, const nlohmann::json::json_pointer& jsonPtr)
{
    // Absent takes priority over unavailable
    if (!status->present.value())
    {
        asyncResp->res.jsonValue[jsonPtr]["Status"]["State"] =
            resource::State::Absent;
    }
    else if (!status->available.value())
    {
        asyncResp->res.jsonValue[jsonPtr]["Status"]["State"] =
            resource::State::UnavailableOffline;
    }
    else
    {
        asyncResp->res.jsonValue[jsonPtr]["Status"]["State"] =
            resource::State::Enabled;
    }
}
inline void getStatusProperty(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::shared_ptr<ResourceStatus>& status, const std::string& service,
    const std::string& path, const std::string& interface,
    const std::string& property, const nlohmann::json::json_pointer& jsonPtr,
    std::function<void(ResourceStatus&, bool)>&& callback)
{
    dbus::utility::getProperty<bool>(
        *crow::connections::systemBus, service, path, interface, property,
        [asyncResp, status, property, jsonPtr, callback{std::move(callback)}](
            const boost::system::error_code& ec, bool value) {
            if (ec)
            {
                if (ec.value() != EBADR)
                {
                    BMCWEB_LOG_ERROR("DBUS response error for {}, ec {}",
                                     property, ec.value());
                    messages::internalError(asyncResp->res);
                    return;
                }
                // Default to true for Enabled/OK
                callback(*status, true);
            }
            else
            {
                callback(*status, value);
            }
            // Only determine Status.State once all properties have been
            // aggregated
            if (status->present.has_value() && status->available.has_value())
            {
                determineResourceState(asyncResp, status, jsonPtr);
            }
        });
}

/*
 * @brief TODO
 *
 * @param[in] asyncResp AsyncResp object to update
 * @param[in] services Map of services to interfaces
 * @param[in] path D-Bus object path
 * @param[in] jsonPtr JSON pointer to where the parent JSON object is
 */
inline void getResourceState(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const dbus::utility::MapperServiceMap& services, const std::string& path,
    const nlohmann::json::json_pointer& jsonPtr)
{
    auto status = std::make_shared<ResourceStatus>();
    const auto findService = [&services](std::string_view targetInterface)
        -> std::optional<std::string_view> {
        for (const auto& [serviceName, interfaces] : services)
        {
            auto it = std::ranges::find(interfaces, targetInterface);
            if (it != interfaces.end())
            {
                return serviceName;
            }
        }
        return std::nullopt;
    };
    const auto presentService =
        findService("xyz.openbmc_project.Inventory.Item");
    const auto availableService =
        findService("xyz.openbmc_project.State.Decorator.Availability");

    if (presentService)
    {
        getStatusProperty(asyncResp, status,
                std::string(*presentService), path,
                "xyz.openbmc_project.Inventory.Item", "Present", jsonPtr,
                [](ResourceStatus& s, bool val) { s.present = val; });
    }
    else
    {
        status->present = true;
    }
    if (availableService)
    {
        getStatusProperty(
            asyncResp, status, std::string(*availableService), path,
            "xyz.openbmc_project.State.Decorator.Availability", "Available",
            jsonPtr, [](ResourceStatus& s, bool val) { s.available = val; });
    }
    else
    {
        status->available = true;
    }
    if (!presentService && !availableService)
    {
        determineResourceState(asyncResp, status, jsonPtr);
    }
}

inline void afterGetResourceHealth(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const nlohmann::json::json_pointer& jsonPtr,
    const boost::system::error_code& ec, bool functional)
{
    if (ec)
    {
        if (ec.value() != EBADR)
        {
            BMCWEB_LOG_ERROR("DBUS response error for {}, ec {}", "Functional",
                             ec.value());
            messages::internalError(asyncResp->res);
            return;
        }
        // Interface missing, default to OK
        functional = true;
    }
    determineResourceHealth(asyncResp, jsonPtr, functional);
}

/*
 * @brief Retrieves the status of the resource's status.health
 *
 * Queries interface:
 * - xyz.openbmc_project.State.Decorator.OperationalStatus::Functional
 */
inline void getResourceHealth(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& service, const std::string& path,
    const nlohmann::json::json_pointer& jsonPtr)
{
    dbus::utility::getProperty<bool>(
        *crow::connections::systemBus, service, path,
        "xyz.openbmc_project.State.Decorator.OperationalStatus", "Functional",
        std::bind_front(afterGetResourceHealth, asyncResp, jsonPtr));
}

/*
 * @brief Retrieves the status.health of the first service that implements
 * State.Decorator.OperationalStatus. If no service implements the interface,
 * it will default to Status.Health OK
 *
 * @param[in] asyncResp AsyncResp object to update
 * @param[in] services Map of services to interfaces
 * @param[in] path D-Bus object path
 * @param[in] jsonPtr JSON pointer to where the parent JSON object is
 */
inline void getResourceHealth(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const dbus::utility::MapperServiceMap& services, const std::string& path,
    const nlohmann::json::json_pointer& jsonPtr)
{
    for (const auto& [serviceName, interfaces] : services)
    {
        for (const auto& interface : interfaces)
        {
            if (interface ==
                "xyz.openbmc_project.State.Decorator.OperationalStatus")
            {
                getResourceHealth(asyncResp, serviceName, path, jsonPtr);
                return;
            }
        }
    }
    asyncResp->res.jsonValue[jsonPtr]["Status"]["Health"] =
        resource::Health::OK;
}

} // namespace resource_utils
} // namespace redfish
