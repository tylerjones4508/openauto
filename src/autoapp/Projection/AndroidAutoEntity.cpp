/*
*  This file is part of openauto project.
*  Copyright (C) 2018 f1x.studio (Michal Szwaj)
*
*  openauto is free software: you can redistribute it and/or modify
*  it under the terms of the GNU General Public License as published by
*  the Free Software Foundation; either version 3 of the License, or
*  (at your option) any later version.

*  openauto is distributed in the hope that it will be useful,
*  but WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*  GNU General Public License for more details.
*
*  You should have received a copy of the GNU General Public License
*  along with openauto. If not, see <http://www.gnu.org/licenses/>.
*/

#include <f1x/aasdk/Messenger/MessageInStream.hpp>
#include <f1x/aasdk/Messenger/MessageOutStream.hpp>
#include <f1x/aasdk/Messenger/Messenger.hpp>
#include <f1x/openauto/autoapp/Projection/AndroidAutoEntity.hpp>
#include <f1x/openauto/Common/Log.hpp>

namespace f1x
{
namespace openauto
{
namespace autoapp
{
namespace projection
{

AndroidAutoEntity::AndroidAutoEntity(boost::asio::io_service& ioService,
                                     aasdk::messenger::ICryptor::Pointer cryptor,
                                     aasdk::transport::ITransport::Pointer transport,
                                     configuration::IConfiguration::Pointer configuration,
                                     IServiceFactory& serviceFactory)
    : strand_(ioService)
    , cryptor_(std::move(cryptor))
    , transport_(std::move(transport))
    , configuration_(std::move(configuration))
    , serviceFactory_(serviceFactory)
    , messenger_(std::make_shared<aasdk::messenger::Messenger>(ioService,
                                                               std::make_shared<aasdk::messenger::MessageInStream>(ioService, transport_, cryptor_),
                                                               std::make_shared<aasdk::messenger::MessageOutStream>(ioService, transport_, cryptor_)))
    , controlServiceChannel_(std::make_shared<aasdk::channel::control::ControlServiceChannel>(strand_, messenger_))
    , eventHandler_(nullptr)
{
}

AndroidAutoEntity::~AndroidAutoEntity()
{
    OPENAUTO_LOG(debug) << "[AndroidAutoEntity] destroy.";
}

void AndroidAutoEntity::start(IAndroidAutoEntityEventHandler& eventHandler)
{
    strand_.dispatch([this, self = this->shared_from_this(), eventHandler = &eventHandler]() {
        OPENAUTO_LOG(info) << "[AndroidAutoEntity] start.";
        eventHandler_ = eventHandler;

        cryptor_->init();

        serviceList_ = serviceFactory_.create(messenger_);
        std::for_each(serviceList_.begin(), serviceList_.end(), std::bind(&IService::start, std::placeholders::_1));

        controlServiceChannel_->receive(this->shared_from_this());
        auto versionRequestPromise = aasdk::channel::SendPromise::defer(strand_);
        versionRequestPromise->then([]() {}, std::bind(&AndroidAutoEntity::onChannelError, this->shared_from_this(), std::placeholders::_1));
        controlServiceChannel_->sendVersionRequest(std::move(versionRequestPromise));
    });
}

void AndroidAutoEntity::stop()
{
    strand_.dispatch([this, self = this->shared_from_this()]() {
        OPENAUTO_LOG(info) << "[AndroidAutoEntity] stop.";
        this->handleStop();
    });
}

void AndroidAutoEntity::onVersionResponse(uint16_t majorCode, uint16_t minorCode, aasdk::proto::enums::VersionResponseStatus::Enum status)
{
    OPENAUTO_LOG(info) << "[AndroidAutoEntity] version response, version: " << majorCode
                       << "." << minorCode
                       << ", status: " << status;

    if(status == aasdk::proto::enums::VersionResponseStatus::MISMATCH)
    {
        OPENAUTO_LOG(error) << "[AndroidAutoEntity] version mismatch.";
        eventHandler_->onAndroidAutoQuit();
    }
    else
    {
        OPENAUTO_LOG(info) << "[AndroidAutoEntity] Begin handshake.";

        try
        {
            cryptor_->doHandshake();

            auto handshakePromise = aasdk::channel::SendPromise::defer(strand_);
            handshakePromise->then([]() {}, std::bind(&AndroidAutoEntity::onChannelError, this->shared_from_this(), std::placeholders::_1));
            controlServiceChannel_->sendHandshake(cryptor_->readHandshakeBuffer(), std::move(handshakePromise));
            controlServiceChannel_->receive(this->shared_from_this());
        }
        catch(const aasdk::error::Error& e)
        {
            this->onChannelError(e);
        }
    }
}

void AndroidAutoEntity::onHandshake(const aasdk::common::DataConstBuffer& payload)
{
    OPENAUTO_LOG(info) << "[AndroidAutoEntity] Handshake, size: " << payload.size;

    try
    {
        cryptor_->writeHandshakeBuffer(payload);

        if(!cryptor_->doHandshake())
        {
            OPENAUTO_LOG(info) << "[AndroidAutoEntity] continue handshake.";

            auto handshakePromise = aasdk::channel::SendPromise::defer(strand_);
            handshakePromise->then([]() {}, std::bind(&AndroidAutoEntity::onChannelError, this->shared_from_this(), std::placeholders::_1));
            controlServiceChannel_->sendHandshake(cryptor_->readHandshakeBuffer(), std::move(handshakePromise));
        }
        else
        {
            OPENAUTO_LOG(info) << "[AndroidAutoEntity] Auth completed.";

            aasdk::proto::messages::AuthCompleteIndication authCompleteIndication;
            authCompleteIndication.set_status(aasdk::proto::enums::Status::OK);

            auto authCompletePromise = aasdk::channel::SendPromise::defer(strand_);
            authCompletePromise->then([]() {}, std::bind(&AndroidAutoEntity::onChannelError, this->shared_from_this(), std::placeholders::_1));
            controlServiceChannel_->sendAuthComplete(authCompleteIndication, std::move(authCompletePromise));
        }

        controlServiceChannel_->receive(this->shared_from_this());
    }
    catch(const aasdk::error::Error& e)
    {
        this->onChannelError(e);
    }
}

void AndroidAutoEntity::onServiceDiscoveryRequest(const aasdk::proto::messages::ServiceDiscoveryRequest& request)
{
    OPENAUTO_LOG(info) << "[AndroidAutoEntity] Discovery request, device name: " << request.device_name()
                       << ", brand: " << request.device_brand();

    aasdk::proto::messages::ServiceDiscoveryResponse serviceDiscoveryResponse;
    serviceDiscoveryResponse.mutable_channels()->Reserve(256);
    serviceDiscoveryResponse.set_head_unit_name("OpenAuto");
    serviceDiscoveryResponse.set_car_model("Universal");
    serviceDiscoveryResponse.set_car_year("2018");
    serviceDiscoveryResponse.set_car_serial("20180301");
    serviceDiscoveryResponse.set_left_hand_drive_vehicle(configuration_->getHandednessOfTrafficType() == configuration::HandednessOfTrafficType::LEFT_HAND_DRIVE);
    serviceDiscoveryResponse.set_headunit_manufacturer("f1x");
    serviceDiscoveryResponse.set_headunit_model("OpenAuto Autoapp");
    serviceDiscoveryResponse.set_sw_build("1");
    serviceDiscoveryResponse.set_sw_version("1.0");
    serviceDiscoveryResponse.set_can_play_native_media_during_vr(false);
    serviceDiscoveryResponse.set_hide_clock(!configuration_->showClock());

    std::for_each(serviceList_.begin(), serviceList_.end(), std::bind(&IService::fillFeatures, std::placeholders::_1, std::ref(serviceDiscoveryResponse)));

    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then([]() {}, std::bind(&AndroidAutoEntity::onChannelError, this->shared_from_this(), std::placeholders::_1));
    controlServiceChannel_->sendServiceDiscoveryResponse(serviceDiscoveryResponse, std::move(promise));
    controlServiceChannel_->receive(this->shared_from_this());
}

void AndroidAutoEntity::onAudioFocusRequest(const aasdk::proto::messages::AudioFocusRequest& request)
{
    OPENAUTO_LOG(info) << "[AndroidAutoEntity] requested audio focus, type: " << request.audio_focus_type();

    aasdk::proto::enums::AudioFocusState::Enum audioFocusState =
            request.audio_focus_type() == aasdk::proto::enums::AudioFocusType::RELEASE ? aasdk::proto::enums::AudioFocusState::LOSS
                                                                                       : aasdk::proto::enums::AudioFocusState::GAIN;

    OPENAUTO_LOG(info) << "[AndroidAutoEntity] audio focus state: " << audioFocusState;

    aasdk::proto::messages::AudioFocusResponse response;
    response.set_audio_focus_state(audioFocusState);

    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then([]() {}, std::bind(&AndroidAutoEntity::onChannelError, this->shared_from_this(), std::placeholders::_1));
    controlServiceChannel_->sendAudioFocusResponse(response, std::move(promise));
    controlServiceChannel_->receive(this->shared_from_this());
}

void AndroidAutoEntity::onShutdownRequest(const aasdk::proto::messages::ShutdownRequest& request)
{
    OPENAUTO_LOG(info) << "[AndroidAutoEntity] Shutdown request, reason: " << request.reason();

    aasdk::proto::messages::ShutdownResponse response;
    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then([this, self = this->shared_from_this()]() {
            eventHandler_->onAndroidAutoQuit();
        },
        std::bind(&AndroidAutoEntity::onChannelError, this->shared_from_this(), std::placeholders::_1));

    controlServiceChannel_->sendShutdownResponse(response, std::move(promise));
    controlServiceChannel_->receive(this->shared_from_this());
}

void AndroidAutoEntity::onShutdownResponse(const aasdk::proto::messages::ShutdownResponse&)
{
    OPENAUTO_LOG(info) << "[AndroidAutoEntity] Shutdown response ";
    eventHandler_->onAndroidAutoQuit();
}

void AndroidAutoEntity::onNavigationFocusRequest(const aasdk::proto::messages::NavigationFocusRequest& request)
{
    OPENAUTO_LOG(info) << "[AndroidAutoEntity] navigation focus request, type: " << request.type();

    aasdk::proto::messages::NavigationFocusResponse response;
    response.set_type(2);

    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then([]() {}, std::bind(&AndroidAutoEntity::onChannelError, this->shared_from_this(), std::placeholders::_1));
    controlServiceChannel_->sendNavigationFocusResponse(response, std::move(promise));
    controlServiceChannel_->receive(this->shared_from_this());
}

void AndroidAutoEntity::onChannelError(const aasdk::error::Error& e)
{
    OPENAUTO_LOG(error) << "[AndroidAutoEntity] channel error: " << e.what();
    eventHandler_->onAndroidAutoQuit();
}

void AndroidAutoEntity::handleStop()
{
    std::for_each(serviceList_.begin(), serviceList_.end(), std::bind(&IService::stop, std::placeholders::_1));

    messenger_->stop();
    cryptor_->deinit();
    transport_->stop();
    eventHandler_ = nullptr;
}

}
}
}
}